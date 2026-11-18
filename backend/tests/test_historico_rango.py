"""Histórico con rango fijo (desde/hasta) para revisar ensayos pasados. Solo SQLite en memoria; no toca Railway."""
import os
os.environ["DEBUG"] = "false"

import unittest
from datetime import datetime, timedelta, timezone

from fastapi import HTTPException
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker

from app.database import Base
from app.models import Dispositivo, Medicion
from app.routers import mediciones as rm

UTC = timezone.utc
T0 = datetime(2026, 11, 13, 23, 0, tzinfo=UTC)


class HistoricoRangoTests(unittest.TestCase):
    def setUp(self):
        self.engine = create_engine("sqlite:///:memory:")
        Base.metadata.create_all(self.engine)
        self.db = sessionmaker(bind=self.engine)()
        disp = Dispositivo(device_id="SM-A", activo=True, conectado=False)
        self.db.add(disp)
        self.db.flush()
        # 3 h de lecturas cada 5 s (2160), como las corridas de metrología cargadas desde la microSD
        for k in range(2160):
            self.db.add(Medicion(
                dispositivo_id=disp.id, device_id="SM-A", timestamp=T0 + timedelta(seconds=5 * k),
                voltaje_rms=120.0, corriente_rms=0.8 if k < 1080 else 1.6, potencia_activa=96.0,
                energia_activa=5000.0 + k * 0.01, boot_id=9, seq=k + 1,
            ))
        self.db.commit()

    def tearDown(self):
        self.db.close()
        self.engine.dispose()

    def historico(self, **kw):
        base = {"device_id": "SM-A", "horas": 24, "limite": 2000, "desde": None, "hasta": None, "db": self.db}
        base.update(kw)
        return rm.obtener_historico(**base)

    def test_rango_corto_devuelve_filas_crudas(self):
        r = self.historico(desde=T0, hasta=T0 + timedelta(minutes=10))
        self.assertEqual(r.resolucion, "cruda")
        self.assertEqual(r.total_mediciones, 120)
        self.assertEqual(r.periodo_inicio, T0)

    def test_rango_largo_agrega_en_el_paso_mas_corto_que_cabe(self):
        r = self.historico(desde=T0, hasta=T0 + timedelta(hours=3))
        self.assertEqual(r.resolucion, "10s")  # 10 800 s / 10 s = 1080 puntos <= 2000
        self.assertEqual(r.total_mediciones, 1080)
        self.assertTrue(all(p.muestras == 2 for p in r.mediciones))
        # el escalón de corriente se conserva
        self.assertAlmostEqual(r.mediciones[-1].corriente_rms, 0.8)
        self.assertAlmostEqual(r.mediciones[0].corriente_rms, 1.6)

    def test_hasta_es_exclusivo_y_sin_zona_es_utc(self):
        r = self.historico(desde=T0.replace(tzinfo=None), hasta=(T0 + timedelta(seconds=50)).replace(tzinfo=None))
        self.assertEqual(r.total_mediciones, 10)

    def test_rango_invalido_o_mayor_a_31_dias(self):
        with self.assertRaises(HTTPException) as e:
            self.historico(desde=T0, hasta=T0)
        self.assertEqual(e.exception.status_code, 422)
        with self.assertRaises(HTTPException) as e:
            self.historico(desde=T0, hasta=T0 + timedelta(days=32))
        self.assertEqual(e.exception.status_code, 422)

    def test_rango_sin_datos_da_404(self):
        with self.assertRaises(HTTPException) as e:
            self.historico(desde=T0 - timedelta(days=2), hasta=T0 - timedelta(days=1))
        self.assertEqual(e.exception.status_code, 404)


if __name__ == "__main__":
    unittest.main()
