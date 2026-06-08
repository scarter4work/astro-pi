/**
 * Parameter panel for configuring the stacking algorithm
 * Features: tooltips, organized sections, visual feedback
 */

import { useState } from 'react';
import { Info, Cpu, Sliders, Image } from 'lucide-react';

interface ParameterPanelProps {
  fusionStrategy: number;
  outlierSigma: number;
  confidenceThreshold: number;
  useGPU: boolean;
  generateConfidenceMap: boolean;
  onFusionStrategyChange: (value: number) => void;
  onOutlierSigmaChange: (value: number) => void;
  onConfidenceThresholdChange: (value: number) => void;
  onUseGPUChange: (value: boolean) => void;
  onGenerateConfidenceMapChange: (value: boolean) => void;
  disabled?: boolean;
}

const FUSION_STRATEGIES = [
  {
    value: 0,
    label: 'MLE',
    description: 'Maximum Likelihood Estimation - best for well-behaved Gaussian noise',
    details: 'Uses the statistical mean for Gaussian distributions. Fast and effective for typical stacking scenarios.',
  },
  {
    value: 1,
    label: 'Confidence Weighted',
    description: 'Weight by inverse variance and quality metrics',
    details: 'Automatically down-weights noisy or artifact-affected pixels. Recommended for most use cases.',
  },
  {
    value: 2,
    label: 'Lucky Imaging',
    description: 'Per-pixel selection from best frame',
    details: 'Selects the sharpest value for each pixel based on local quality. Best for planetary or high-resolution imaging.',
  },
  {
    value: 3,
    label: 'Multi-Scale',
    description: 'Different strategies at different spatial frequencies',
    details: 'Preserves fine detail while maximizing SNR in smooth regions. Advanced technique for experienced users.',
  },
];

interface TooltipProps {
  content: string;
  children: React.ReactNode;
}

function Tooltip({ content, children }: TooltipProps) {
  const [show, setShow] = useState(false);

  return (
    <div className="relative inline-block">
      <div
        onMouseEnter={() => setShow(true)}
        onMouseLeave={() => setShow(false)}
      >
        {children}
      </div>
      {show && (
        <div className="absolute z-10 bottom-full left-1/2 -translate-x-1/2 mb-2 px-3 py-2 text-xs bg-gray-900 border border-gray-600 rounded shadow-lg max-w-xs whitespace-normal">
          {content}
          <div className="absolute top-full left-1/2 -translate-x-1/2 -mt-1 border-4 border-transparent border-t-gray-600" />
        </div>
      )}
    </div>
  );
}

interface SectionProps {
  title: string;
  icon: React.ReactNode;
  children: React.ReactNode;
}

function Section({ title, icon, children }: SectionProps) {
  return (
    <div className="space-y-3">
      <div className="flex items-center gap-2 text-sm font-medium text-gray-300 border-b border-gray-700 pb-2">
        {icon}
        {title}
      </div>
      {children}
    </div>
  );
}

export function ParameterPanel({
  fusionStrategy,
  outlierSigma,
  confidenceThreshold,
  useGPU,
  generateConfidenceMap,
  onFusionStrategyChange,
  onOutlierSigmaChange,
  onConfidenceThresholdChange,
  onUseGPUChange,
  onGenerateConfidenceMapChange,
  disabled,
}: ParameterPanelProps) {
  const [showDetails, setShowDetails] = useState(false);
  const selectedStrategy = FUSION_STRATEGIES.find(s => s.value === fusionStrategy);

  return (
    <div className="bg-gray-800 rounded-lg p-4">
      <h2 className="text-lg font-semibold mb-4">Parameters</h2>

      <div className="space-y-6">
        {/* Fusion Strategy Section */}
        <Section title="Fusion Strategy" icon={<Sliders size={16} />}>
          <div>
            <div className="flex items-center gap-2 mb-1">
              <label className="block text-sm font-medium">Algorithm</label>
              <Tooltip content="How pixel values from multiple frames are combined into the final image">
                <Info size={14} className="text-gray-500 cursor-help" />
              </Tooltip>
            </div>
            <select
              value={fusionStrategy}
              onChange={(e) => onFusionStrategyChange(Number(e.target.value))}
              disabled={disabled}
              className="w-full bg-gray-700 border border-gray-600 rounded px-3 py-2 text-sm disabled:opacity-50 focus:border-blue-500 focus:ring-1 focus:ring-blue-500 outline-none transition-colors"
            >
              {FUSION_STRATEGIES.map((s) => (
                <option key={s.value} value={s.value}>
                  {s.label}
                </option>
              ))}
            </select>
            <p className="text-xs text-gray-400 mt-1">
              {selectedStrategy?.description}
            </p>
            {showDetails && selectedStrategy && (
              <p className="text-xs text-gray-500 mt-1 p-2 bg-gray-750 rounded">
                {selectedStrategy.details}
              </p>
            )}
            <button
              onClick={() => setShowDetails(!showDetails)}
              className="text-xs text-blue-400 hover:text-blue-300 mt-1"
            >
              {showDetails ? 'Hide details' : 'Show details'}
            </button>
          </div>
        </Section>

        {/* Statistical Parameters Section */}
        <Section title="Statistical Parameters" icon={<Sliders size={16} />}>
          {/* Outlier Sigma */}
          <div>
            <div className="flex items-center justify-between mb-1">
              <div className="flex items-center gap-2">
                <label className="text-sm font-medium">Outlier Rejection</label>
                <Tooltip content="Pixels deviating more than this many standard deviations from the mean are flagged as potential artifacts">
                  <Info size={14} className="text-gray-500 cursor-help" />
                </Tooltip>
              </div>
              <span className="text-sm text-blue-400 font-mono">{outlierSigma.toFixed(1)}σ</span>
            </div>
            <input
              type="range"
              min="1.0"
              max="10.0"
              step="0.5"
              value={outlierSigma}
              onChange={(e) => onOutlierSigmaChange(Number(e.target.value))}
              disabled={disabled}
              className="w-full h-2 bg-gray-700 rounded-lg appearance-none cursor-pointer disabled:opacity-50 accent-blue-500"
            />
            <div className="flex justify-between text-xs text-gray-500 mt-1">
              <span>Aggressive (1σ)</span>
              <span>Permissive (10σ)</span>
            </div>
          </div>

          {/* Confidence Threshold */}
          <div>
            <div className="flex items-center justify-between mb-1">
              <div className="flex items-center gap-2">
                <label className="text-sm font-medium">Confidence Threshold</label>
                <Tooltip content="Pixels with confidence below this value may be masked or flagged in the output">
                  <Info size={14} className="text-gray-500 cursor-help" />
                </Tooltip>
              </div>
              <span className="text-sm text-blue-400 font-mono">{(confidenceThreshold * 100).toFixed(0)}%</span>
            </div>
            <input
              type="range"
              min="0"
              max="0.5"
              step="0.01"
              value={confidenceThreshold}
              onChange={(e) => onConfidenceThresholdChange(Number(e.target.value))}
              disabled={disabled}
              className="w-full h-2 bg-gray-700 rounded-lg appearance-none cursor-pointer disabled:opacity-50 accent-blue-500"
            />
            <div className="flex justify-between text-xs text-gray-500 mt-1">
              <span>Include all (0%)</span>
              <span>High quality only (50%)</span>
            </div>
          </div>
        </Section>

        {/* Output Options Section */}
        <Section title="Output Options" icon={<Image size={16} />}>
          <label className="flex items-start gap-3 cursor-pointer group">
            <input
              type="checkbox"
              checked={generateConfidenceMap}
              onChange={(e) => onGenerateConfidenceMapChange(e.target.checked)}
              disabled={disabled}
              className="mt-0.5 rounded bg-gray-700 border-gray-600 text-blue-500 focus:ring-blue-500 focus:ring-offset-0"
            />
            <div>
              <span className="text-sm group-hover:text-white transition-colors">Generate Confidence Map</span>
              <p className="text-xs text-gray-500 mt-0.5">
                Create a separate image showing per-pixel confidence values
              </p>
            </div>
          </label>
        </Section>

        {/* Performance Section */}
        <Section title="Performance" icon={<Cpu size={16} />}>
          <label className="flex items-start gap-3 cursor-pointer group">
            <input
              type="checkbox"
              checked={useGPU}
              onChange={(e) => onUseGPUChange(e.target.checked)}
              disabled={disabled}
              className="mt-0.5 rounded bg-gray-700 border-gray-600 text-blue-500 focus:ring-blue-500 focus:ring-offset-0"
            />
            <div>
              <span className="text-sm group-hover:text-white transition-colors">Use GPU Acceleration</span>
              <p className="text-xs text-gray-500 mt-0.5">
                CUDA acceleration for NVIDIA GPUs. Falls back to CPU if unavailable.
              </p>
            </div>
          </label>

          {useGPU && (
            <div className="text-xs text-gray-500 p-2 bg-gray-750 rounded flex items-center gap-2">
              <Cpu size={14} className="text-green-400" />
              <span>GPU acceleration enabled</span>
            </div>
          )}
        </Section>
      </div>
    </div>
  );
}
