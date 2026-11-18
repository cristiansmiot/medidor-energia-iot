/**
 * Selector de rango fijo para la pestaña Histórico: un ensayo registrado o un rango personalizado.
 * Las fechas del rango personalizado se escriben en hora de Bogotá (UTC−5) y se envían en UTC.
 */
import { useState } from 'react';
import ENSAYOS from '../../data/ensayos';
import { formatBogotaDateTime } from '../../utils/dateTime';

const BOGOTA_OFFSET = '-05:00';

const aUtcIso = (local) => (local ? new Date(`${local}:00${BOGOTA_OFFSET}`).toISOString() : null);

const SelectorEnsayos = ({ rango, onCambio }) => {
  const [ensayoId, setEnsayoId] = useState('');
  const [desdeLocal, setDesdeLocal] = useState('');
  const [hastaLocal, setHastaLocal] = useState('');
  const [error, setError] = useState('');

  const elegirEnsayo = (id) => {
    setEnsayoId(id);
    setError('');
    const e = ENSAYOS.find((x) => x.id === id);
    onCambio(e ? { desde: e.desde, hasta: e.hasta, nombre: e.nombre } : null);
  };

  const aplicarPersonalizado = () => {
    const desde = aUtcIso(desdeLocal);
    const hasta = aUtcIso(hastaLocal);
    if (!desde || !hasta || desde >= hasta) {
      setError('Indique un inicio anterior al fin.');
      return;
    }
    if (new Date(hasta) - new Date(desde) > 31 * 24 * 3600 * 1000) {
      setError('El rango admite hasta 31 días.');
      return;
    }
    setError('');
    setEnsayoId('');
    onCambio({ desde, hasta, nombre: 'Rango personalizado' });
  };

  const limpiar = () => {
    setEnsayoId('');
    setError('');
    onCambio(null);
  };

  return (
    <div className="bg-white rounded-xl border border-gray-200 p-4">
      <div className="flex flex-wrap items-end gap-3">
        <label className="flex flex-col text-xs text-gray-500 min-w-[16rem] flex-1">
          Ensayo registrado
          <select
            value={ensayoId}
            onChange={(ev) => elegirEnsayo(ev.target.value)}
            className="mt-1 rounded-lg border border-gray-300 px-2 py-1.5 text-sm text-gray-800"
          >
            <option value="">— Ventana reciente —</option>
            {ENSAYOS.map((e) => (
              <option key={e.id} value={e.id}>{e.nombre}</option>
            ))}
          </select>
        </label>
        <label className="flex flex-col text-xs text-gray-500">
          Desde (hora Bogotá)
          <input
            type="datetime-local"
            value={desdeLocal}
            onChange={(ev) => setDesdeLocal(ev.target.value)}
            className="mt-1 rounded-lg border border-gray-300 px-2 py-1.5 text-sm text-gray-800"
          />
        </label>
        <label className="flex flex-col text-xs text-gray-500">
          Hasta (hora Bogotá)
          <input
            type="datetime-local"
            value={hastaLocal}
            onChange={(ev) => setHastaLocal(ev.target.value)}
            className="mt-1 rounded-lg border border-gray-300 px-2 py-1.5 text-sm text-gray-800"
          />
        </label>
        <button
          onClick={aplicarPersonalizado}
          className="px-3 py-1.5 rounded-lg text-sm font-medium bg-blue-600 text-white hover:bg-blue-700"
        >
          Ver rango
        </button>
        {rango && (
          <button
            onClick={limpiar}
            className="px-3 py-1.5 rounded-lg text-sm font-medium bg-gray-100 text-gray-600 hover:bg-gray-200"
          >
            Volver a la ventana reciente
          </button>
        )}
      </div>
      {error && <p className="mt-2 text-xs text-red-600">{error}</p>}
      {rango && (
        <p className="mt-2 text-xs text-gray-500">
          {rango.nombre}: {formatBogotaDateTime(rango.desde)} a {formatBogotaDateTime(rango.hasta)}
        </p>
      )}
    </div>
  );
};

export default SelectorEnsayos;
