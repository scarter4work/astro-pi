import { useState, useCallback, useRef, useEffect } from 'react';
import { Header } from './components/Header';
import { ControlPanel } from './components/ControlPanel';
import { PreviewCanvas } from './components/PreviewCanvas';
import { Histogram } from './components/Histogram';
import { ImageLoader } from './components/ImageLoader';
import { OTSParams, DEFAULT_OTS_PARAMS, applyOTS, getHistogramData } from './engines/OptimalTransportStretch';
import { SASParams, DEFAULT_SAS_PARAMS, applySAS } from './engines/StarletArctanStretch';

export type AlgorithmType = 'ots' | 'sas';

export interface ImageState {
  original: ImageData | null;
  processed: ImageData | null;
  fileName: string;
}

export default function App() {
  // Algorithm selection
  const [algorithm, setAlgorithm] = useState<AlgorithmType>('ots');

  // Parameters
  const [otsParams, setOTSParams] = useState<OTSParams>(DEFAULT_OTS_PARAMS);
  const [sasParams, setSASParams] = useState<SASParams>(DEFAULT_SAS_PARAMS);

  // Image state
  const [imageState, setImageState] = useState<ImageState>({
    original: null,
    processed: null,
    fileName: ''
  });

  // UI state
  const [isProcessing, setIsProcessing] = useState(false);
  const [showOriginal, setShowOriginal] = useState(false);
  const [zoom, setZoom] = useState(1);
  const [histogramData, setHistogramData] = useState<{ source: Float32Array | null; result: Float32Array | null }>({
    source: null,
    result: null
  });

  const processingTimeoutRef = useRef<number | null>(null);

  // Process image with current parameters
  const processImage = useCallback(() => {
    if (!imageState.original) return;

    setIsProcessing(true);

    // Use requestAnimationFrame to allow UI to update
    requestAnimationFrame(() => {
      try {
        let result: ImageData;

        if (algorithm === 'ots') {
          result = applyOTS(imageState.original!, otsParams);
        } else {
          result = applySAS(imageState.original!, sasParams);
        }

        setImageState(prev => ({ ...prev, processed: result }));

        // Update histogram
        const sourceHist = getHistogramData(imageState.original!);
        const resultHist = getHistogramData(result);
        setHistogramData({
          source: sourceHist.source,
          result: resultHist.source
        });
      } catch (error) {
        console.error('Processing error:', error);
      } finally {
        setIsProcessing(false);
      }
    });
  }, [imageState.original, algorithm, otsParams, sasParams]);

  // Debounced processing on parameter change
  useEffect(() => {
    if (!imageState.original) return;

    if (processingTimeoutRef.current) {
      clearTimeout(processingTimeoutRef.current);
    }

    processingTimeoutRef.current = window.setTimeout(() => {
      processImage();
    }, 100);

    return () => {
      if (processingTimeoutRef.current) {
        clearTimeout(processingTimeoutRef.current);
      }
    };
  }, [otsParams, sasParams, algorithm, processImage]);

  // Handle image load
  const handleImageLoad = useCallback((imageData: ImageData, fileName: string) => {
    setImageState({
      original: imageData,
      processed: null,
      fileName
    });

    // Compute initial histogram
    const sourceHist = getHistogramData(imageData);
    setHistogramData({
      source: sourceHist.source,
      result: null
    });

    setZoom(1);
  }, []);

  // Update OTS params
  const updateOTSParam = useCallback(<K extends keyof OTSParams>(key: K, value: OTSParams[K]) => {
    setOTSParams(prev => ({ ...prev, [key]: value }));
  }, []);

  // Update SAS params
  const updateSASParam = useCallback(<K extends keyof SASParams>(key: K, value: SASParams[K]) => {
    setSASParams(prev => ({ ...prev, [key]: value }));
  }, []);

  // Reset to defaults
  const resetParams = useCallback(() => {
    if (algorithm === 'ots') {
      setOTSParams(DEFAULT_OTS_PARAMS);
    } else {
      setSASParams(DEFAULT_SAS_PARAMS);
    }
  }, [algorithm]);

  // Export processed image
  const exportImage = useCallback(() => {
    if (!imageState.processed) return;

    const canvas = document.createElement('canvas');
    canvas.width = imageState.processed.width;
    canvas.height = imageState.processed.height;
    const ctx = canvas.getContext('2d')!;
    ctx.putImageData(imageState.processed, 0, 0);

    const link = document.createElement('a');
    const baseName = imageState.fileName.replace(/\.[^/.]+$/, '');
    link.download = `${baseName}_${algorithm}_stretched.png`;
    link.href = canvas.toDataURL('image/png');
    link.click();
  }, [imageState.processed, imageState.fileName, algorithm]);

  return (
    <div className="app-container">
      <Header
        algorithm={algorithm}
        onAlgorithmChange={setAlgorithm}
        onExport={exportImage}
        hasImage={!!imageState.processed}
        isProcessing={isProcessing}
      />

      <div className="main-content">
        <aside className="control-sidebar">
          <ControlPanel
            algorithm={algorithm}
            otsParams={otsParams}
            sasParams={sasParams}
            onUpdateOTS={updateOTSParam}
            onUpdateSAS={updateSASParam}
            onReset={resetParams}
          />

          <Histogram
            sourceData={histogramData.source}
            resultData={histogramData.result}
          />
        </aside>

        <main className="preview-area">
          {!imageState.original ? (
            <ImageLoader onImageLoad={handleImageLoad} />
          ) : (
            <PreviewCanvas
              imageData={showOriginal ? imageState.original : imageState.processed || imageState.original}
              zoom={zoom}
              onZoomChange={setZoom}
              showOriginal={showOriginal}
              onToggleOriginal={() => setShowOriginal(!showOriginal)}
              isProcessing={isProcessing}
              fileName={imageState.fileName}
            />
          )}
        </main>
      </div>

      <style>{`
        .app-container {
          display: flex;
          flex-direction: column;
          height: 100vh;
          background: var(--bg-primary);
        }

        .main-content {
          display: flex;
          flex: 1;
          overflow: hidden;
        }

        .control-sidebar {
          width: 340px;
          min-width: 340px;
          display: flex;
          flex-direction: column;
          background: var(--bg-secondary);
          border-right: 1px solid var(--border-subtle);
          overflow-y: auto;
        }

        .preview-area {
          flex: 1;
          display: flex;
          flex-direction: column;
          background: var(--bg-primary);
          overflow: hidden;
        }
      `}</style>
    </div>
  );
}
