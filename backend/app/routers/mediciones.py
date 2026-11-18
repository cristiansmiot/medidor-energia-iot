"""Mediciones - CRUD de mediciones de energía"""
import csv
import io

from fastapi import APIRouter, Depends, HTTPException, Query, Response, status
from sqlalchemy.orm import Session
from sqlalchemy import func, desc, asc
from typing import List, Optional
from datetime import datetime, timedelta, timezone

from app.database import get_db
from app.models.dispositivo import Dispositivo
from app.models.medicion import Medicion
from app.models.usuario import Usuario
from app.schemas.medicion import (
    MedicionCreate, MedicionResponse, MedicionResumen,
    MedicionHistorico, MedicionPunto,
)
from app.services.auth import (
    get_current_user, obtener_dispositivos_asignados, require_operador_or_admin,
)

router = APIRouter(prefix="/mediciones", tags=["Mediciones"])

# Exportación CSV (E7). Solo lectura; exige sesión aunque otras consultas de
# este router sigan abiertas. 31 días a una lectura por minuto son 44 640 filas.
EXPORT_MAX_DIAS = 31
EXPORT_MAX_FILAS = 50_000
EXPORT_COLUMNAS = (
    ("timestamp_utc", Medicion.timestamp),
    ("device_id", Medicion.device_id),
    ("boot_id", Medicion.boot_id),
    ("seq", Medicion.seq),
    ("voltaje_rms_v", Medicion.voltaje_rms),
    ("corriente_rms_a", Medicion.corriente_rms),
    ("potencia_activa_w", Medicion.potencia_activa),
    ("potencia_reactiva_var", Medicion.potencia_reactiva),
    ("potencia_aparente_va", Medicion.potencia_aparente),
    ("factor_potencia", Medicion.factor_potencia),
    ("frecuencia_hz", Medicion.frecuencia),
    ("energia_activa_wh", Medicion.energia_activa),
    ("time_valid", Medicion.time_valid),
    ("ts_device_utc", Medicion.ts_device),
    ("ts_server_utc", Medicion.ts_server),
    ("fw_version", Medicion.fw_version),
)


@router.get("/", response_model=List[MedicionResponse])
def listar_mediciones(skip: int = 0, limit: int = 100, device_id: Optional[str] = None, db: Session = Depends(get_db)):
    query = db.query(Medicion)
    if device_id:
        query = query.filter(Medicion.device_id == device_id.strip().upper())
    return query.order_by(desc(Medicion.timestamp)).offset(skip).limit(limit).all()


@router.get("/{medicion_id}", response_model=MedicionResponse)
def obtener_medicion(medicion_id: int, db: Session = Depends(get_db)):
    medicion = db.query(Medicion).filter(Medicion.id == medicion_id).first()
    if not medicion:
        raise HTTPException(status_code=404, detail=f"Medición {medicion_id} no encontrada")
    return medicion


@router.post("/", response_model=MedicionResponse, status_code=status.HTTP_201_CREATED)
def crear_medicion(
    data: MedicionCreate,
    db: Session = Depends(get_db),
    _usuario: Usuario = Depends(require_operador_or_admin),
):
    # La vía normal de ingesta es MQTT (autenticada en el broker). Este POST
    # existe para pruebas y carga manual: sin candado, cualquiera podía
    # inyectar mediciones falsas a nombre de un medidor real.
    dispositivo = db.query(Dispositivo).filter(Dispositivo.device_id == data.device_id).first()
    if not dispositivo:
        dispositivo = Dispositivo(device_id=data.device_id, nombre=f"Auto-registrado: {data.device_id}")
        db.add(dispositivo)
        db.commit()
        db.refresh(dispositivo)
    if not dispositivo.activo:
        dispositivo.activo = True  # retiro lógico revertido: el equipo volvió a reportar
    dispositivo.conectado = True
    dispositivo.ultima_conexion = datetime.utcnow()
    nueva = Medicion(
        dispositivo_id=dispositivo.id, device_id=data.device_id, voltaje_rms=data.voltaje_rms,
        corriente_rms=data.corriente_rms, potencia_activa=data.potencia_activa,
        potencia_reactiva=data.potencia_reactiva, potencia_aparente=data.potencia_aparente,
        factor_potencia=data.factor_potencia, frecuencia=data.frecuencia, thd_voltaje=data.thd_voltaje,
        thd_corriente=data.thd_corriente, energia_activa=data.energia_activa,
        energia_reactiva=data.energia_reactiva, timestamp=data.timestamp or datetime.utcnow()
    )
    db.add(nueva)
    db.commit()
    db.refresh(nueva)
    return nueva


@router.get("/{device_id}/ultima", response_model=MedicionResponse)
def obtener_ultima_medicion(device_id: str, db: Session = Depends(get_db)):
    device_id = device_id.strip().upper()
    medicion = db.query(Medicion).filter(Medicion.device_id == device_id).order_by(desc(Medicion.timestamp)).first()
    if not medicion:
        raise HTTPException(status_code=404, detail=f"No hay mediciones para '{device_id}'")
    return medicion


@router.get("/{device_id}/resumen", response_model=MedicionResumen)
def obtener_resumen(device_id: str, db: Session = Depends(get_db)):
    device_id = device_id.strip().upper()
    medicion = db.query(Medicion).filter(Medicion.device_id == device_id).order_by(desc(Medicion.timestamp)).first()
    if not medicion:
        raise HTTPException(status_code=404, detail=f"No hay mediciones para '{device_id}'")
    return MedicionResumen(
        device_id=medicion.device_id, voltaje=f"{medicion.voltaje_rms:.1f} V",
        corriente=f"{medicion.corriente_rms:.3f} A", potencia=f"{medicion.potencia_activa:.1f} W",
        factor_potencia=f"{medicion.factor_potencia:.2f}" if medicion.factor_potencia else "N/A",
        energia_kwh=f"{medicion.energia_activa:.3f} kWh" if medicion.energia_activa else "0.000 kWh",
        timestamp=medicion.timestamp
    )


PASOS_AGREGACION_S = (10, 15, 20, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 21600, 86400)
RANGO_FIJO_MAX = timedelta(days=31)


def _agrupar(filas, bucket_s: int, device_id: str) -> list:
    """Promedia las filas (ts, v, i, p, fp, e) en intervalos de bucket_s segundos, del más reciente al más antiguo."""
    # Agrupación en memoria: a escala de prototipo (pocos nodos, 1/min) la ventana anual son cientos de miles de
    # tuplas como máximo, y evita SQL específico por dialecto (date_trunc no existe en SQLite).
    buckets: dict = {}
    for ts, v, i, p, fp, e in filas:
        clave = int(ts.timestamp()) // bucket_s
        b = buckets.setdefault(clave, {"n": 0, "v": 0.0, "i": 0.0, "p": 0.0, "fp": 0.0, "e": None})
        b["n"] += 1
        b["v"] += v or 0.0
        b["i"] += i or 0.0
        b["p"] += p or 0.0
        b["fp"] += fp or 0.0
        b["e"] = e if e is not None else b["e"]  # contador acumulado: vale el último

    puntos = []
    for clave in sorted(buckets.keys(), reverse=True):
        b = buckets[clave]
        n = b["n"]
        puntos.append(MedicionPunto(
            device_id=device_id,
            timestamp=datetime.fromtimestamp(clave * bucket_s, tz=timezone.utc),
            voltaje_rms=round(b["v"] / n, 2),
            corriente_rms=round(b["i"] / n, 4),
            potencia_activa=round(b["p"] / n, 2),
            factor_potencia=round(b["fp"] / n, 3),
            energia_activa=b["e"],
            muestras=n,
        ))
    return puntos


@router.get("/{device_id}/historico", response_model=MedicionHistorico)
def obtener_historico(
    device_id: str,
    horas: int = Query(24, ge=1, le=8760, description="Ventana hacia atrás (hasta 1 año)"),
    limite: int = Query(2000, ge=10, le=2000),
    desde: Optional[datetime] = Query(
        None, description="Inicio de un rango fijo, ISO 8601 (sin zona = UTC). Con 'desde' o 'hasta' se ignora 'horas'"),
    hasta: Optional[datetime] = Query(None, description="Fin exclusivo del rango fijo. Por defecto, 24 h después de 'desde'"),
    db: Session = Depends(get_db),
):
    """
    Serie histórica de un medidor.

    Ventana relativa (`horas`), con resolución adaptativa:
    - hasta 24 h: filas crudas (una por publicación del nodo);
    - hasta 48 h: promedio cada 5 min;
    - hasta ~31 días: promedio por hora;
    - más allá: promedio por día.

    Rango fijo (`desde`/`hasta`, hasta 31 días), para revisar un ensayo pasado: filas crudas si caben en `limite`;
    si no, promedios en el intervalo más corto que deja la serie dentro de `limite` puntos.
    """
    device_id = device_id.strip().upper()
    rango_fijo = desde is not None or hasta is not None
    if rango_fijo:
        desde = _a_utc(desde)
        hasta = _a_utc(hasta) or (desde + timedelta(hours=24) if desde else datetime.now(timezone.utc))
        desde = desde or hasta - timedelta(hours=24)
        if desde >= hasta:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                                detail="'desde' debe ser anterior a 'hasta'")
        if hasta - desde > RANGO_FIJO_MAX:
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                                detail="El rango fijo admite hasta 31 días")
        filtro = (Medicion.device_id == device_id, Medicion.timestamp >= desde, Medicion.timestamp < hasta)
    else:
        hasta = datetime.utcnow()
        desde = hasta - timedelta(hours=horas)
        filtro = (Medicion.device_id == device_id, Medicion.timestamp >= desde)

    stats = db.query(
        func.avg(Medicion.voltaje_rms).label('v_avg'), func.min(Medicion.voltaje_rms).label('v_min'),
        func.max(Medicion.voltaje_rms).label('v_max'), func.avg(Medicion.potencia_activa).label('p_avg'),
        func.max(Medicion.potencia_activa).label('p_max'), func.count(Medicion.id).label('n'),
    ).filter(*filtro).first()

    columnas = (Medicion.timestamp, Medicion.voltaje_rms, Medicion.corriente_rms,
                Medicion.potencia_activa, Medicion.factor_potencia, Medicion.energia_activa)

    if rango_fijo and (stats.n or 0) > limite:
        span_s = (hasta - desde).total_seconds()
        bucket_s = next((p for p in PASOS_AGREGACION_S if span_s / p <= limite), PASOS_AGREGACION_S[-1])
        resolucion = f"{bucket_s}s"
        filas = db.query(*columnas).filter(*filtro).order_by(asc(Medicion.timestamp)).all()
        puntos = _agrupar(filas, bucket_s, device_id)
    elif rango_fijo or horas <= 24:
        resolucion = "cruda"
        crudas = (
            db.query(Medicion).filter(*filtro)
            .order_by(desc(Medicion.timestamp)).limit(limite).all()
        )
        if not crudas:
            raise HTTPException(status_code=404, detail=f"No hay mediciones para '{device_id}'")
        puntos = [MedicionPunto.model_validate(m) for m in crudas]
    else:
        if horas <= 48:
            resolucion = "5min"
            bucket_s = 300
        elif horas <= 744:
            resolucion = "hora"
            bucket_s = 3600
        else:
            resolucion = "dia"
            bucket_s = 86400
        filas = db.query(*columnas).filter(*filtro).order_by(asc(Medicion.timestamp)).all()
        if not filas:
            raise HTTPException(status_code=404, detail=f"No hay mediciones para '{device_id}'")
        puntos = _agrupar(filas, bucket_s, device_id)

    if not puntos:
        raise HTTPException(status_code=404, detail=f"No hay mediciones para '{device_id}'")

    return MedicionHistorico(
        device_id=device_id, periodo_inicio=desde, periodo_fin=hasta,
        total_mediciones=len(puntos), resolucion=resolucion,
        estadisticas={"voltaje_promedio": round(stats.v_avg or 0, 2), "voltaje_minimo": round(stats.v_min or 0, 2),
                      "voltaje_maximo": round(stats.v_max or 0, 2), "potencia_promedio": round(stats.p_avg or 0, 2),
                      "potencia_maxima": round(stats.p_max or 0, 2)},
        mediciones=puntos
    )


def _a_utc(dt: Optional[datetime]) -> Optional[datetime]:
    """Fecha sin zona = UTC (así guarda SQLite); con zona, se convierte a UTC."""
    if dt is None:
        return None
    return dt.replace(tzinfo=timezone.utc) if dt.tzinfo is None else dt.astimezone(timezone.utc)


def _celda(valor) -> str:
    if valor is None:
        return ""
    if isinstance(valor, datetime):
        return _a_utc(valor).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    if isinstance(valor, bool):
        return "1" if valor else "0"
    return str(valor)


def generar_csv_mediciones(db: Session, device_id: str, desde: datetime, hasta: datetime) -> str:
    """CSV de las lecturas de un medidor en [desde, hasta), en orden de tiempo.

    Lanza HTTPException 422 si el intervalo tiene más de EXPORT_MAX_FILAS
    lecturas: se pide reducirlo en lugar de truncar sin avisar.
    """
    filas = (
        db.query(*(col for _, col in EXPORT_COLUMNAS))
        .filter(Medicion.device_id == device_id, Medicion.timestamp >= desde, Medicion.timestamp < hasta)
        .order_by(asc(Medicion.timestamp), asc(Medicion.id))
        .limit(EXPORT_MAX_FILAS + 1)
        .all()
    )
    if len(filas) > EXPORT_MAX_FILAS:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail=f"El intervalo tiene más de {EXPORT_MAX_FILAS} lecturas; reduzca el rango",
        )
    salida = io.StringIO()
    escritor = csv.writer(salida, lineterminator="\n")
    escritor.writerow([nombre for nombre, _ in EXPORT_COLUMNAS])
    for fila in filas:
        escritor.writerow([_celda(v) for v in fila])
    return salida.getvalue()


@router.get("/{device_id}/exportar")
def exportar_mediciones(
    device_id: str,
    desde: Optional[datetime] = Query(None, description="Inicio, ISO 8601 (sin zona = UTC). Por defecto, 24 h antes de 'hasta'"),
    hasta: Optional[datetime] = Query(None, description="Fin exclusivo, ISO 8601 (sin zona = UTC). Por defecto, ahora"),
    db: Session = Depends(get_db),
    usuario: Usuario = Depends(get_current_user),
):
    """Exporta en CSV las lecturas crudas de un medidor (máximo 31 días).

    Requiere sesión. El visualizador solo puede exportar sus medidores
    asignados. No modifica la base de datos.
    """
    device_id = device_id.strip().upper()
    asignados = obtener_dispositivos_asignados(usuario, db)
    if asignados is not None and device_id not in asignados:
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="No tiene acceso a este medidor")
    hasta = _a_utc(hasta) or datetime.now(timezone.utc)
    desde = _a_utc(desde) or hasta - timedelta(hours=24)
    if desde >= hasta:
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                            detail="'desde' debe ser anterior a 'hasta'")
    if hasta - desde > timedelta(days=EXPORT_MAX_DIAS):
        raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                            detail=f"El rango máximo de exportación es de {EXPORT_MAX_DIAS} días")
    if not db.query(Dispositivo.id).filter(Dispositivo.device_id == device_id).first():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail=f"Medidor '{device_id}' no encontrado")

    contenido = generar_csv_mediciones(db, device_id, desde, hasta)
    nombre = f"mediciones_{device_id}_{desde:%Y%m%dT%H%M}Z_{hasta:%Y%m%dT%H%M}Z.csv"
    return Response(
        content=contenido,
        media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": f'attachment; filename="{nombre}"', "Cache-Control": "no-store"},
    )


@router.get("/{device_id}/reconciliacion")
def obtener_reconciliacion(
    device_id: str,
    horas: int = Query(default=24, ge=1, le=168, description="Ventana de tiempo (1-168h)"),
    db: Session = Depends(get_db),
):
    """
    Reconciliación de energía: compara la energía acumulada reportada por
    el dispositivo (energia_activa, valor del NVS del firmware) contra
    la integral de potencia calculada por la plataforma (∑ P·Δt).

    Devuelve el delta en Wh y el porcentaje de discrepancia para detectar
    pérdidas de paquetes, errores de integración o desvíos de calibración.
    """
    device_id = device_id.strip().upper()
    desde = datetime.now(timezone.utc) - timedelta(hours=horas)

    mediciones = (
        db.query(Medicion)
        .filter(Medicion.device_id == device_id, Medicion.timestamp >= desde)
        .order_by(asc(Medicion.timestamp))
        .all()
    )

    if len(mediciones) < 2:
        raise HTTPException(
            status_code=404,
            detail=f"Se necesitan al menos 2 mediciones para '{device_id}' en las últimas {horas}h",
        )

    primera = mediciones[0]
    ultima = mediciones[-1]

    # Energía según el dispositivo (NVS counter acumulado): diferencia entre
    # la primera y la última medición de la ventana. Si el contador cayó
    # (reinicio del firmware) se detecta y se reporta como reset_detectado.
    e_disp_inicio = primera.energia_activa or 0.0
    e_disp_fin = ultima.energia_activa or 0.0
    reset_detectado = e_disp_fin < e_disp_inicio
    energia_dispositivo_wh = (e_disp_fin - e_disp_inicio) if not reset_detectado else None

    # Integral de potencia: ∑ P(t) × Δt (regla del rectángulo izquierdo)
    energia_plataforma_wh = 0.0
    for i in range(1, len(mediciones)):
        t1 = mediciones[i - 1].timestamp
        t2 = mediciones[i].timestamp
        p_w = mediciones[i - 1].potencia_activa or 0.0
        dt_h = (t2 - t1).total_seconds() / 3600.0
        energia_plataforma_wh += p_w * dt_h

    energia_plataforma_wh = round(energia_plataforma_wh, 4)

    discrepancia_wh = None
    discrepancia_pct = None
    if energia_dispositivo_wh is not None and energia_dispositivo_wh > 0:
        discrepancia_wh = round(energia_dispositivo_wh - energia_plataforma_wh, 4)
        discrepancia_pct = round(abs(discrepancia_wh) / energia_dispositivo_wh * 100, 2)

    return {
        "device_id": device_id,
        "ventana_horas": horas,
        "total_mediciones": len(mediciones),
        "periodo_inicio": primera.timestamp,
        "periodo_fin": ultima.timestamp,
        "energia_dispositivo_wh": round(energia_dispositivo_wh, 4) if energia_dispositivo_wh is not None else None,
        "energia_plataforma_wh": energia_plataforma_wh,
        "discrepancia_wh": discrepancia_wh,
        "discrepancia_pct": discrepancia_pct,
        "reset_detectado": reset_detectado,
        "nota": (
            "Reset de firmware detectado en la ventana — energía dispositivo no disponible"
            if reset_detectado else
            f"Intervalo promedio: {round((ultima.timestamp - primera.timestamp).total_seconds() / max(len(mediciones)-1,1), 1)}s"
        ),
    }
