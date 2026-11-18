"""Exportación CSV de mediciones (E7). Solo SQLite en memoria; no toca Railway."""
import os
os.environ["DEBUG"] = "false"

import csv
import io
import unittest
from datetime import datetime, timedelta, timezone

from fastapi import HTTPException
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker

from app.database import Base
from app.models import Dispositivo, Medicion
from app.models.usuario import Usuario, UsuarioDispositivo
from app.routers import mediciones as rm

UTC = timezone.utc
T0 = datetime(2026, 11, 14, 12, 0, tzinfo=UTC)


class ExportarMedicionesTests(unittest.TestCase):
    def setUp(self):
        self.engine = create_engine("sqlite:///:memory:")
        Base.metadata.create_all(self.engine)
        self.db = sessionmaker(bind=self.engine)()
        self.disp = Dispositivo(device_id="SM-A", activo=True, conectado=False)
        otro = Dispositivo(device_id="SM-B", activo=True, conectado=False)
        self.db.add_all([self.disp, otro])
        self.db.flush()
        for k in range(5):
            self.db.add(Medicion(
                dispositivo_id=self.disp.id, device_id="SM-A", timestamp=T0 + timedelta(minutes=k),
                voltaje_rms=120.0 + k, corriente_rms=0.5, potencia_activa=60.0, energia_activa=1000.0 + k,
                boot_id=7, seq=k + 1, time_valid=True, fw_version="rc6.4",
            ))
        self.db.add(Medicion(dispositivo_id=otro.id, device_id="SM-B", timestamp=T0,
                             voltaje_rms=110.0, corriente_rms=0.1, potencia_activa=10.0))
        self.admin = Usuario(email="a@x", nombre="A", apellido="A", password_hash="x", rol="super_admin", activo=True)
        self.visor = Usuario(email="v@x", nombre="V", apellido="V", password_hash="x", rol="visualizador", activo=True)
        self.db.add_all([self.admin, self.visor])
        self.db.flush()
        self.db.add(UsuarioDispositivo(usuario_id=self.visor.id, device_id="SM-B"))
        self.db.commit()

    def tearDown(self):
        self.db.close()
        self.engine.dispose()

    def exportar(self, usuario, device_id="SM-A", desde=T0, hasta=T0 + timedelta(minutes=10)):
        return rm.exportar_mediciones(device_id=device_id, desde=desde, hasta=hasta, db=self.db, usuario=usuario)

    def filas(self, resp):
        return list(csv.DictReader(io.StringIO(resp.body.decode("utf-8"))))

    def test_csv_con_encabezado_orden_y_solo_el_medidor_pedido(self):
        resp = self.exportar(self.admin, device_id=" sm-a ")
        self.assertTrue(resp.media_type.startswith("text/csv"))
        self.assertIn('attachment; filename="mediciones_SM-A_', resp.headers["content-disposition"])
        filas = self.filas(resp)
        self.assertEqual([f["seq"] for f in filas], ["1", "2", "3", "4", "5"])
        self.assertEqual({f["device_id"] for f in filas}, {"SM-A"})
        self.assertEqual(filas[0]["timestamp_utc"], "2026-11-14T12:00:00.000Z")
        self.assertEqual(filas[2]["energia_activa_wh"], "1002.0")
        self.assertEqual(filas[0]["time_valid"], "1")

    def test_intervalo_semiabierto(self):
        filas = self.filas(self.exportar(self.admin, desde=T0 + timedelta(minutes=1), hasta=T0 + timedelta(minutes=3)))
        self.assertEqual([f["seq"] for f in filas], ["2", "3"])

    def test_fecha_sin_zona_se_toma_como_utc(self):
        filas = self.filas(self.exportar(self.admin, desde=datetime(2026, 11, 14, 12, 4), hasta=datetime(2026, 11, 14, 13, 0)))
        self.assertEqual([f["seq"] for f in filas], ["5"])

    def test_visualizador_solo_sus_medidores(self):
        with self.assertRaises(HTTPException) as cm:
            self.exportar(self.visor, device_id="SM-A")
        self.assertEqual(cm.exception.status_code, 403)
        self.assertEqual(len(self.filas(self.exportar(self.visor, device_id="SM-B"))), 1)

    def test_rangos_invalidos(self):
        with self.assertRaises(HTTPException) as cm:
            self.exportar(self.admin, desde=T0, hasta=T0)
        self.assertEqual(cm.exception.status_code, 422)
        with self.assertRaises(HTTPException) as cm:
            self.exportar(self.admin, desde=T0, hasta=T0 + timedelta(days=32))
        self.assertEqual(cm.exception.status_code, 422)

    def test_medidor_inexistente(self):
        with self.assertRaises(HTTPException) as cm:
            self.exportar(self.admin, device_id="SM-Z")
        self.assertEqual(cm.exception.status_code, 404)

    def test_demasiadas_filas_no_se_trunca_en_silencio(self):
        original = rm.EXPORT_MAX_FILAS
        rm.EXPORT_MAX_FILAS = 3
        try:
            with self.assertRaises(HTTPException) as cm:
                self.exportar(self.admin)
            self.assertEqual(cm.exception.status_code, 422)
        finally:
            rm.EXPORT_MAX_FILAS = original

    def test_la_ruta_exige_sesion(self):
        from fastapi.params import Depends as DependsParam
        import inspect
        dep = inspect.signature(rm.exportar_mediciones).parameters["usuario"].default
        self.assertIsInstance(dep, DependsParam)
        self.assertIs(dep.dependency, rm.get_current_user)


if __name__ == "__main__":
    unittest.main()
