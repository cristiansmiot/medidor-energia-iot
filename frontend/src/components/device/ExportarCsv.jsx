import { useState } from 'react';
import { Download } from 'lucide-react';
import { medicionesAPI } from '../../services/api';

// Bogotá no tiene horario de verano: UTC-5 todo el año.
const OFFSET_BOGOTA = '-05:00';
const MAX_DIAS = 31;

// Fecha y hora de Bogotá en el formato del input datetime-local (YYYY-MM-DDTHH:mm).
const aInputBogota = (fecha) =>
  new Date(fecha.getTime() - 5 * 3600 * 1000).toISOString().slice(0, 16);

/**
 * Exporta a CSV las lecturas crudas del medidor en un rango de fechas
 * (hora de Bogotá, máximo 31 días). Requiere sesión; el backend limita al
 * visualizador a sus medidores asignados.
 */
const ExportarCsv = ({ deviceId }) => {
  const ahora = new Date();
  const [desde, setDesde] = useState(aInputBogota(new Date(ahora.getTime() - 24 * 3600 * 1000)));
  const [hasta, setHasta] = useState(aInputBogota(ahora));
  const [descargando, setDescargando] = useState(false);
  const [mensaje, setMensaje] = useState('');

  const descargar = async () => {
    setMensaje('');
    const d = new Date(`${desde}:00${OFFSET_BOGOTA}`);
    const h = new Date(`${hasta}:00${OFFSET_BOGOTA}`);
    if (Number.isNaN(d.getTime()) || Number.isNaN(h.getTime()) || d >= h) {
      setMensaje('La fecha inicial debe ser anterior a la final.');
      return;
    }
    if (h - d > MAX_DIAS * 24 * 3600 * 1000) {
      setMensaje(`El rango máximo es de ${MAX_DIAS} días.`);
      return;
    }
    setDescargando(true);
    try {
      const { blob, nombre } = await medicionesAPI.exportarCsv(deviceId, d.toISOString(), h.toISOString());
      const url = URL.createObjectURL(blob);
      const enlace = document.createElement('a');
      enlace.href = url;
      enlace.download = nombre;
      document.body.appendChild(enlace);
      enlace.click();
      enlace.remove();
      URL.revokeObjectURL(url);
    } catch (err) {
      setMensaje(err.message || 'No se pudo exportar.');
    } finally {
      setDescargando(false);
    }
  };

  return (
    <div className="bg-white rounded-xl shadow-md p-6">
      <h3 className="text-lg font-semibold text-gray-700 mb-1">Exportar lecturas</h3>
      <p className="text-sm text-gray-500 mb-4">
        CSV con todas las lecturas del rango (hora de Bogotá, máximo {MAX_DIAS} días). Tiempos del archivo en UTC.
      </p>
      <div className="flex flex-wrap items-end gap-4">
        <label className="text-sm text-gray-600">
          Desde
          <input
            type="datetime-local"
            value={desde}
            onChange={(e) => setDesde(e.target.value)}
            className="block mt-1 border border-gray-300 rounded-lg px-3 py-2 text-sm"
          />
        </label>
        <label className="text-sm text-gray-600">
          Hasta
          <input
            type="datetime-local"
            value={hasta}
            onChange={(e) => setHasta(e.target.value)}
            className="block mt-1 border border-gray-300 rounded-lg px-3 py-2 text-sm"
          />
        </label>
        <button
          type="button"
          onClick={descargar}
          disabled={descargando}
          className="inline-flex items-center gap-2 px-4 py-2 rounded-lg bg-blue-600 text-white text-sm font-medium hover:bg-blue-700 disabled:opacity-50"
        >
          <Download size={16} />
          {descargando ? 'Exportando…' : 'Descargar CSV'}
        </button>
      </div>
      {mensaje && <p className="text-sm text-red-600 mt-3">{mensaje}</p>}
    </div>
  );
};

export default ExportarCsv;
