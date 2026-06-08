import { AlgorithmType } from '../App';
import { OTSParams, ObjectType } from '../engines/OptimalTransportStretch';
import { SASParams, SAS_PRESETS } from '../engines/StarletArctanStretch';

interface SliderProps {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onChange: (value: number) => void;
  format?: (v: number) => string;
  description?: string;
}

function Slider({ label, value, min, max, step, onChange, format, description }: SliderProps) {
  const displayValue = format ? format(value) : value.toFixed(step < 1 ? 2 : 0);

  return (
    <div className="slider-control">
      <div className="slider-header">
        <span className="slider-label">{label}</span>
        <span className="slider-value">{displayValue}</span>
      </div>
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={value}
        onChange={(e) => onChange(parseFloat(e.target.value))}
      />
      <div className="slider-range">
        <span>{format ? format(min) : min}</span>
        <span>{format ? format(max) : max}</span>
      </div>
      {description && <p className="slider-description">{description}</p>}
    </div>
  );
}

interface ControlPanelProps {
  algorithm: AlgorithmType;
  otsParams: OTSParams;
  sasParams: SASParams;
  onUpdateOTS: <K extends keyof OTSParams>(key: K, value: OTSParams[K]) => void;
  onUpdateSAS: <K extends keyof SASParams>(key: K, value: SASParams[K]) => void;
  onReset: () => void;
}

export function ControlPanel({
  algorithm,
  otsParams,
  sasParams,
  onUpdateOTS,
  onUpdateSAS,
  onReset
}: ControlPanelProps) {
  const handlePreset = (presetName: keyof typeof SAS_PRESETS) => {
    const preset = SAS_PRESETS[presetName];
    Object.entries(preset).forEach(([key, value]) => {
      onUpdateSAS(key as keyof SASParams, value as SASParams[keyof SASParams]);
    });
  };

  return (
    <div className="control-panel">
      <div className="panel-header">
        <svg width="18" height="18" viewBox="0 0 18 18" fill="none">
          <path d="M9 3 L9 15 M3 9 L15 9" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
          <circle cx="9" cy="9" r="2" stroke="currentColor" strokeWidth="1.5" fill="none" />
        </svg>
        <span>Parameters</span>
        <button className="btn btn-icon reset-btn" onClick={onReset} title="Reset to defaults">
          <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
            <path d="M2 7 A5 5 0 1 1 3 10 M2 7 L2 10 L5 10" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" fill="none" />
          </svg>
        </button>
      </div>

      <div className="panel-content">
        {algorithm === 'ots' ? (
          <OTSControls params={otsParams} onUpdate={onUpdateOTS} />
        ) : (
          <SASControls params={sasParams} onUpdate={onUpdateSAS} onPreset={handlePreset} />
        )}
      </div>

      <style>{`
        .control-panel {
          display: flex;
          flex-direction: column;
          flex: 1;
          min-height: 0;
        }

        .panel-header {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
          padding: var(--space-md);
          border-bottom: 1px solid var(--border-subtle);
          color: var(--text-primary);
          font-weight: 600;
        }

        .panel-header .reset-btn {
          margin-left: auto;
        }

        .panel-content {
          flex: 1;
          overflow-y: auto;
          padding: var(--space-md);
        }

        .slider-control {
          margin-bottom: var(--space-lg);
        }

        .slider-header {
          display: flex;
          justify-content: space-between;
          align-items: center;
          margin-bottom: var(--space-xs);
        }

        .slider-label {
          font-size: 12px;
          font-weight: 500;
          color: var(--text-secondary);
          text-transform: uppercase;
          letter-spacing: 0.3px;
        }

        .slider-value {
          font-family: 'JetBrains Mono', monospace;
          font-size: 13px;
          color: var(--accent-secondary);
          background: var(--bg-tertiary);
          padding: 2px 8px;
          border-radius: 4px;
        }

        .slider-range {
          display: flex;
          justify-content: space-between;
          margin-top: 4px;
          font-size: 10px;
          color: var(--text-muted);
        }

        .slider-description {
          margin-top: var(--space-xs);
          font-size: 11px;
          color: var(--text-muted);
          line-height: 1.4;
        }

        .section-title {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
          margin: var(--space-lg) 0 var(--space-md);
          padding-bottom: var(--space-sm);
          border-bottom: 1px solid var(--border-subtle);
          font-size: 12px;
          font-weight: 600;
          color: var(--text-secondary);
          text-transform: uppercase;
          letter-spacing: 0.5px;
        }

        .section-title:first-child {
          margin-top: 0;
        }

        .checkbox-control {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
          margin-bottom: var(--space-md);
          cursor: pointer;
          font-size: 13px;
          color: var(--text-secondary);
        }

        .checkbox-control:hover {
          color: var(--text-primary);
        }

        .select-control {
          margin-bottom: var(--space-lg);
        }

        .select-label {
          display: block;
          margin-bottom: var(--space-xs);
          font-size: 12px;
          font-weight: 500;
          color: var(--text-secondary);
          text-transform: uppercase;
          letter-spacing: 0.3px;
        }

        .preset-buttons {
          display: grid;
          grid-template-columns: 1fr 1fr;
          gap: var(--space-sm);
          margin-bottom: var(--space-lg);
        }

        .preset-btn {
          padding: var(--space-sm);
          background: var(--bg-tertiary);
          border: 1px solid var(--border-subtle);
          border-radius: var(--radius-sm);
          font-size: 11px;
          font-weight: 500;
          color: var(--text-secondary);
          cursor: pointer;
          transition: all var(--transition-fast);
        }

        .preset-btn:hover {
          background: var(--bg-hover);
          color: var(--text-primary);
          border-color: var(--accent-primary);
        }
      `}</style>
    </div>
  );
}

// OTS Controls Component
function OTSControls({
  params,
  onUpdate
}: {
  params: OTSParams;
  onUpdate: <K extends keyof OTSParams>(key: K, value: OTSParams[K]) => void;
}) {
  const objectTypes: { value: ObjectType; label: string; desc: string }[] = [
    { value: 'nebula', label: 'Emission Nebula', desc: 'Optimized for H-alpha, OIII' },
    { value: 'galaxy', label: 'Galaxy', desc: 'Preserves core-to-halo gradient' },
    { value: 'starCluster', label: 'Star Cluster', desc: 'Emphasizes stellar magnitude range' },
    { value: 'darkNebula', label: 'Dark Nebula', desc: 'Inverts tonal emphasis' }
  ];

  return (
    <>
      <div className="section-title">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
          <circle cx="7" cy="7" r="5" stroke="currentColor" strokeWidth="1.5" fill="none" />
          <circle cx="7" cy="7" r="2" fill="currentColor" />
        </svg>
        Object Type
      </div>

      <div className="select-control">
        <label className="select-label">Target Distribution</label>
        <select
          value={params.objectType}
          onChange={(e) => onUpdate('objectType', e.target.value as ObjectType)}
        >
          {objectTypes.map(({ value, label, desc }) => (
            <option key={value} value={value} title={desc}>
              {label}
            </option>
          ))}
        </select>
        <p className="slider-description" style={{ marginTop: '8px' }}>
          {objectTypes.find(t => t.value === params.objectType)?.desc}
        </p>
      </div>

      <div className="section-title">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
          <rect x="2" y="10" width="2" height="2" fill="currentColor" />
          <rect x="6" y="6" width="2" height="6" fill="currentColor" />
          <rect x="10" y="2" width="2" height="10" fill="currentColor" />
        </svg>
        Stretch Parameters
      </div>

      <Slider
        label="Background Target"
        value={params.backgroundTarget}
        min={0.05}
        max={0.30}
        step={0.01}
        onChange={(v) => onUpdate('backgroundTarget', v)}
        format={(v) => `${(v * 100).toFixed(0)}%`}
        description="Target background brightness level"
      />

      <Slider
        label="Stretch Intensity"
        value={params.stretchIntensity}
        min={0}
        max={1}
        step={0.01}
        onChange={(v) => onUpdate('stretchIntensity', v)}
        format={(v) => `${(v * 100).toFixed(0)}%`}
        description="Blend between original and full optimal transport"
      />

      <Slider
        label="Highlight Protection"
        value={params.protectHighlights}
        min={0}
        max={1}
        step={0.01}
        onChange={(v) => onUpdate('protectHighlights', v)}
        format={(v) => `${(v * 100).toFixed(0)}%`}
        description="Prevents bright regions from clipping"
      />

      <div className="section-title">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
          <circle cx="4" cy="7" r="2" fill="#ef4444" />
          <circle cx="7" cy="7" r="2" fill="#22c55e" />
          <circle cx="10" cy="7" r="2" fill="#3b82f6" />
        </svg>
        Color Handling
      </div>

      <label className="checkbox-control">
        <input
          type="checkbox"
          checked={params.preserveColor}
          onChange={(e) => onUpdate('preserveColor', e.target.checked)}
        />
        Preserve chrominance (apply to luminance only)
      </label>
    </>
  );
}

// SAS Controls Component
function SASControls({
  params,
  onUpdate,
  onPreset
}: {
  params: SASParams;
  onUpdate: <K extends keyof SASParams>(key: K, value: SASParams[K]) => void;
  onPreset: (name: keyof typeof SAS_PRESETS) => void;
}) {
  return (
    <>
      <div className="section-title">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
          <rect x="2" y="2" width="10" height="10" rx="2" stroke="currentColor" strokeWidth="1.5" fill="none" />
        </svg>
        Presets
      </div>

      <div className="preset-buttons">
        <button className="preset-btn" onClick={() => onPreset('emissionNebula')}>
          Emission Nebula
        </button>
        <button className="preset-btn" onClick={() => onPreset('galaxy')}>
          Galaxy
        </button>
        <button className="preset-btn" onClick={() => onPreset('starCluster')}>
          Star Cluster
        </button>
        <button className="preset-btn" onClick={() => onPreset('faintExtended')}>
          Faint Extended
        </button>
      </div>

      <div className="section-title">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
          <path d="M2 10 Q5 4 7 7 Q9 10 12 4" stroke="currentColor" strokeWidth="1.5" fill="none" />
        </svg>
        Decomposition
      </div>

      <Slider
        label="Number of Scales"
        value={params.numScales}
        min={4}
        max={8}
        step={1}
        onChange={(v) => onUpdate('numScales', v)}
        description="More scales = finer control over structure sizes"
      />

      <div className="section-title">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
          <path d="M2 12 L2 7 L5 7 L5 4 L8 4 L8 2 L12 2" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" fill="none" />
        </svg>
        Scale Gains
      </div>

      <Slider
        label="Fine Scales (1-2)"
        value={params.fineScaleGain}
        min={0.5}
        max={2.0}
        step={0.1}
        onChange={(v) => onUpdate('fineScaleGain', v)}
        format={(v) => `${v.toFixed(1)}x`}
        description="Stars and fine noise"
      />

      <Slider
        label="Mid Scales (3-4)"
        value={params.midScaleGain}
        min={1.0}
        max={5.0}
        step={0.1}
        onChange={(v) => onUpdate('midScaleGain', v)}
        format={(v) => `${v.toFixed(1)}x`}
        description="Fine structure and detail"
      />

      <Slider
        label="Coarse Scales (5-6)"
        value={params.coarseScaleGain}
        min={1.0}
        max={8.0}
        step={0.1}
        onChange={(v) => onUpdate('coarseScaleGain', v)}
        format={(v) => `${v.toFixed(1)}x`}
        description="Diffuse nebulosity and large structures"
      />

      <div className="section-title">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
          <path d="M2 10 C4 10 4 4 7 4 C10 4 10 10 12 10" stroke="currentColor" strokeWidth="1.5" fill="none" />
        </svg>
        Compression
      </div>

      <Slider
        label="Background Target"
        value={params.backgroundTarget}
        min={0.05}
        max={0.25}
        step={0.01}
        onChange={(v) => onUpdate('backgroundTarget', v)}
        format={(v) => `${(v * 100).toFixed(0)}%`}
      />

      <Slider
        label="Compression Alpha"
        value={params.compressionAlpha}
        min={1}
        max={20}
        step={0.5}
        onChange={(v) => onUpdate('compressionAlpha', v)}
        description="Higher = more aggressive dynamic range compression"
      />

      <Slider
        label="Highlight Protection"
        value={params.highlightProtection}
        min={0}
        max={1}
        step={0.01}
        onChange={(v) => onUpdate('highlightProtection', v)}
        format={(v) => `${(v * 100).toFixed(0)}%`}
        description="Reduces gain in bright regions to prevent halos"
      />

      <div className="section-title">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
          <path d="M2 7 L12 7" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeDasharray="2 2" />
          <circle cx="7" cy="7" r="4" stroke="currentColor" strokeWidth="1.5" fill="none" />
        </svg>
        Noise & Background
      </div>

      <Slider
        label="Noise Threshold"
        value={params.noiseThreshold}
        min={0}
        max={0.01}
        step={0.0001}
        onChange={(v) => onUpdate('noiseThreshold', v)}
        format={(v) => v.toFixed(4)}
        description="Threshold for noise suppression in fine scales"
      />

      <label className="checkbox-control">
        <input
          type="checkbox"
          checked={params.flattenBackground}
          onChange={(e) => onUpdate('flattenBackground', e.target.checked)}
        />
        Flatten background (suppress coarsest scale)
      </label>

      <label className="checkbox-control">
        <input
          type="checkbox"
          checked={params.preserveColor}
          onChange={(e) => onUpdate('preserveColor', e.target.checked)}
        />
        Preserve chrominance
      </label>
    </>
  );
}
