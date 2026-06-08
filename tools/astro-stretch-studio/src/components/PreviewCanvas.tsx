import React, { useRef, useEffect, useState, useCallback } from 'react';

interface PreviewCanvasProps {
  imageData: ImageData;
  zoom: number;
  onZoomChange: (zoom: number) => void;
  showOriginal: boolean;
  onToggleOriginal: () => void;
  isProcessing: boolean;
  fileName: string;
}

export function PreviewCanvas({
  imageData,
  zoom,
  onZoomChange,
  showOriginal,
  onToggleOriginal,
  isProcessing,
  fileName
}: PreviewCanvasProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  const [pan, setPan] = useState({ x: 0, y: 0 });
  const [isDragging, setIsDragging] = useState(false);
  const [dragStart, setDragStart] = useState({ x: 0, y: 0 });
  const [fitMode, setFitMode] = useState<'fit' | 'fill' | 'actual'>('fit');

  // Calculate fit zoom
  const calculateFitZoom = useCallback(() => {
    if (!containerRef.current || !imageData) return 1;
    const container = containerRef.current;
    const padding = 40;
    const availWidth = container.clientWidth - padding;
    const availHeight = container.clientHeight - padding;
    const scaleX = availWidth / imageData.width;
    const scaleY = availHeight / imageData.height;
    return Math.min(scaleX, scaleY, 1);
  }, [imageData]);

  // Draw image to canvas
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !imageData) return;

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    // Set canvas size
    canvas.width = imageData.width;
    canvas.height = imageData.height;

    // Draw image
    ctx.putImageData(imageData, 0, 0);
  }, [imageData]);

  // Handle fit mode change
  useEffect(() => {
    if (fitMode === 'fit') {
      onZoomChange(calculateFitZoom());
      setPan({ x: 0, y: 0 });
    } else if (fitMode === 'actual') {
      onZoomChange(1);
      setPan({ x: 0, y: 0 });
    }
  }, [fitMode, calculateFitZoom, onZoomChange]);

  // Handle wheel zoom
  const handleWheel = useCallback((e: React.WheelEvent) => {
    e.preventDefault();
    const delta = e.deltaY > 0 ? 0.9 : 1.1;
    const newZoom = Math.max(0.1, Math.min(10, zoom * delta));
    onZoomChange(newZoom);
    setFitMode('fill'); // Exit fit mode on manual zoom
  }, [zoom, onZoomChange]);

  // Handle mouse down for panning
  const handleMouseDown = useCallback((e: React.MouseEvent) => {
    if (e.button === 0) { // Left click only
      setIsDragging(true);
      setDragStart({ x: e.clientX - pan.x, y: e.clientY - pan.y });
    }
  }, [pan]);

  // Handle mouse move for panning
  const handleMouseMove = useCallback((e: React.MouseEvent) => {
    if (isDragging) {
      setPan({
        x: e.clientX - dragStart.x,
        y: e.clientY - dragStart.y
      });
    }
  }, [isDragging, dragStart]);

  // Handle mouse up
  const handleMouseUp = useCallback(() => {
    setIsDragging(false);
  }, []);

  // Zoom controls
  const zoomIn = () => {
    const newZoom = Math.min(10, zoom * 1.25);
    onZoomChange(newZoom);
    setFitMode('fill');
  };

  const zoomOut = () => {
    const newZoom = Math.max(0.1, zoom * 0.8);
    onZoomChange(newZoom);
    setFitMode('fill');
  };

  const zoomFit = () => {
    setFitMode('fit');
  };

  const zoom100 = () => {
    setFitMode('actual');
  };

  return (
    <div className="preview-container">
      {/* Toolbar */}
      <div className="preview-toolbar">
        <div className="toolbar-left">
          <span className="file-name">{fileName}</span>
          <span className="image-info">
            {imageData.width} x {imageData.height}
          </span>
        </div>

        <div className="toolbar-center">
          <button
            className={`toggle-btn ${showOriginal ? 'active' : ''}`}
            onMouseDown={onToggleOriginal}
            onMouseUp={onToggleOriginal}
            onMouseLeave={() => showOriginal && onToggleOriginal()}
            title="Hold to view original"
          >
            <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
              <circle cx="7" cy="7" r="5" stroke="currentColor" strokeWidth="1.5" fill="none" />
              <path d="M4.5 7 L6 8.5 L9.5 5" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
            </svg>
            {showOriginal ? 'Original' : 'Processed'}
          </button>
        </div>

        <div className="toolbar-right">
          <div className="zoom-controls">
            <button className="btn btn-icon" onClick={zoomOut} title="Zoom Out">
              <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
                <circle cx="6" cy="6" r="4" stroke="currentColor" strokeWidth="1.5" fill="none" />
                <path d="M9 9 L12 12" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
                <path d="M4 6 L8 6" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
              </svg>
            </button>

            <span className="zoom-value">{(zoom * 100).toFixed(0)}%</span>

            <button className="btn btn-icon" onClick={zoomIn} title="Zoom In">
              <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
                <circle cx="6" cy="6" r="4" stroke="currentColor" strokeWidth="1.5" fill="none" />
                <path d="M9 9 L12 12" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
                <path d="M4 6 L8 6 M6 4 L6 8" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
              </svg>
            </button>

            <div className="zoom-divider" />

            <button
              className={`btn btn-icon ${fitMode === 'fit' ? 'active' : ''}`}
              onClick={zoomFit}
              title="Fit to Window"
            >
              <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
                <rect x="2" y="2" width="10" height="10" rx="1" stroke="currentColor" strokeWidth="1.5" fill="none" />
                <path d="M4 4 L6 6 M10 4 L8 6 M4 10 L6 8 M10 10 L8 8" stroke="currentColor" strokeWidth="1" strokeLinecap="round" />
              </svg>
            </button>

            <button
              className={`btn btn-icon ${fitMode === 'actual' ? 'active' : ''}`}
              onClick={zoom100}
              title="100%"
            >
              1:1
            </button>
          </div>
        </div>
      </div>

      {/* Canvas Area */}
      <div
        ref={containerRef}
        className={`canvas-container ${isDragging ? 'dragging' : ''}`}
        onWheel={handleWheel}
        onMouseDown={handleMouseDown}
        onMouseMove={handleMouseMove}
        onMouseUp={handleMouseUp}
        onMouseLeave={handleMouseUp}
      >
        {/* Checkerboard background */}
        <div className="canvas-background" />

        <canvas
          ref={canvasRef}
          className="preview-canvas"
          style={{
            transform: `translate(${pan.x}px, ${pan.y}px) scale(${zoom})`,
            opacity: isProcessing ? 0.7 : 1
          }}
        />

        {isProcessing && (
          <div className="processing-overlay">
            <div className="processing-spinner" />
          </div>
        )}
      </div>

      <style>{`
        .preview-container {
          display: flex;
          flex-direction: column;
          height: 100%;
          background: var(--bg-primary);
        }

        .preview-toolbar {
          display: flex;
          align-items: center;
          justify-content: space-between;
          height: 44px;
          padding: 0 var(--space-md);
          background: var(--bg-secondary);
          border-bottom: 1px solid var(--border-subtle);
        }

        .toolbar-left,
        .toolbar-center,
        .toolbar-right {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
        }

        .toolbar-left {
          flex: 1;
        }

        .toolbar-right {
          flex: 1;
          justify-content: flex-end;
        }

        .file-name {
          font-size: 13px;
          font-weight: 500;
          color: var(--text-primary);
          max-width: 200px;
          overflow: hidden;
          text-overflow: ellipsis;
          white-space: nowrap;
        }

        .image-info {
          font-size: 11px;
          color: var(--text-muted);
          padding: 2px 8px;
          background: var(--bg-tertiary);
          border-radius: 4px;
        }

        .toggle-btn {
          display: flex;
          align-items: center;
          gap: var(--space-xs);
          padding: 6px 12px;
          background: var(--bg-tertiary);
          border: 1px solid var(--border-subtle);
          border-radius: var(--radius-sm);
          font-size: 12px;
          font-weight: 500;
          color: var(--text-secondary);
          cursor: pointer;
          transition: all var(--transition-fast);
          user-select: none;
        }

        .toggle-btn:hover {
          background: var(--bg-hover);
          color: var(--text-primary);
        }

        .toggle-btn.active {
          background: var(--accent-primary);
          border-color: var(--accent-primary);
          color: white;
        }

        .zoom-controls {
          display: flex;
          align-items: center;
          gap: var(--space-xs);
        }

        .zoom-value {
          min-width: 50px;
          text-align: center;
          font-family: 'JetBrains Mono', monospace;
          font-size: 12px;
          color: var(--text-secondary);
        }

        .zoom-divider {
          width: 1px;
          height: 20px;
          background: var(--border-subtle);
          margin: 0 var(--space-xs);
        }

        .btn-icon.active {
          background: var(--accent-primary);
          color: white;
        }

        .canvas-container {
          flex: 1;
          position: relative;
          overflow: hidden;
          display: flex;
          align-items: center;
          justify-content: center;
          cursor: grab;
        }

        .canvas-container.dragging {
          cursor: grabbing;
        }

        .canvas-background {
          position: absolute;
          inset: 0;
          background-image:
            linear-gradient(45deg, var(--bg-tertiary) 25%, transparent 25%),
            linear-gradient(-45deg, var(--bg-tertiary) 25%, transparent 25%),
            linear-gradient(45deg, transparent 75%, var(--bg-tertiary) 75%),
            linear-gradient(-45deg, transparent 75%, var(--bg-tertiary) 75%);
          background-size: 20px 20px;
          background-position: 0 0, 0 10px, 10px -10px, -10px 0px;
          opacity: 0.5;
        }

        .preview-canvas {
          position: relative;
          image-rendering: pixelated;
          box-shadow: var(--shadow-lg);
          transition: opacity var(--transition-fast);
        }

        .processing-overlay {
          position: absolute;
          inset: 0;
          display: flex;
          align-items: center;
          justify-content: center;
          background: rgba(0, 0, 0, 0.3);
          backdrop-filter: blur(2px);
        }

        .processing-spinner {
          width: 40px;
          height: 40px;
          border: 3px solid var(--bg-tertiary);
          border-top-color: var(--accent-primary);
          border-radius: 50%;
          animation: spin 1s linear infinite;
        }

        @keyframes spin {
          to { transform: rotate(360deg); }
        }
      `}</style>
    </div>
  );
}
