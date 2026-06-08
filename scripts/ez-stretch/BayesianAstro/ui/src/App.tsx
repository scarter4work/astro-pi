/**
 * BayesianAstro React UI
 *
 * Main application component for the embedded Qt WebView interface
 */

import { useBridge } from './hooks/useBridge';
import { FileList } from './components/FileList';
import { ParameterPanel } from './components/ParameterPanel';
import { ProgressPanel } from './components/ProgressPanel';

export default function App() {
  const bridge = useBridge();

  return (
    <div className="min-h-screen bg-gray-900 text-gray-100 p-4">
      <header className="mb-6">
        <h1 className="text-2xl font-bold text-blue-400">BayesianAstro</h1>
        <p className="text-gray-400 text-sm">
          Distribution-aware image stacking with per-pixel confidence scoring
        </p>
        {!bridge.connected && (
          <p className="text-yellow-500 text-xs mt-1">Development mode - Qt bridge not connected</p>
        )}
      </header>

      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4">
        {/* Left panel - File list */}
        <div className="lg:col-span-2">
          <FileList
            files={bridge.files}
            onAddFiles={bridge.addFiles}
            onRemoveFile={bridge.removeFile}
            onClearFiles={bridge.clearFiles}
            disabled={bridge.processing.isProcessing}
          />
        </div>

        {/* Right panel - Parameters */}
        <div className="space-y-4">
          <ParameterPanel
            fusionStrategy={bridge.fusionStrategy}
            outlierSigma={bridge.outlierSigma}
            confidenceThreshold={bridge.confidenceThreshold}
            useGPU={bridge.useGPU}
            generateConfidenceMap={bridge.generateConfidenceMap}
            onFusionStrategyChange={bridge.setFusionStrategy}
            onOutlierSigmaChange={bridge.setOutlierSigma}
            onConfidenceThresholdChange={bridge.setConfidenceThreshold}
            onUseGPUChange={bridge.setUseGPU}
            onGenerateConfidenceMapChange={bridge.setGenerateConfidenceMap}
            disabled={bridge.processing.isProcessing}
          />

          <ProgressPanel
            isProcessing={bridge.processing.isProcessing}
            progress={bridge.processing.progress}
            status={bridge.processing.status}
            onExecute={bridge.execute}
            canExecute={bridge.files.length > 0 && !bridge.processing.isProcessing}
          />
        </div>
      </div>
    </div>
  );
}
