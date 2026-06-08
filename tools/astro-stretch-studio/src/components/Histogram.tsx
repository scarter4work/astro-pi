import { useRef, useEffect } from 'react';

interface HistogramProps {
  sourceData: Float32Array | null;
  resultData: Float32Array | null;
}

export function Histogram({ sourceData, resultData }: HistogramProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const width = canvas.width;
    const height = canvas.height;
    const padding = { top: 10, right: 10, bottom: 20, left: 10 };
    const chartWidth = width - padding.left - padding.right;
    const chartHeight = height - padding.top - padding.bottom;

    // Clear canvas
    ctx.fillStyle = '#12121a';
    ctx.fillRect(0, 0, width, height);

    // Draw grid lines
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.05)';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const y = padding.top + (chartHeight / 4) * i;
      ctx.beginPath();
      ctx.moveTo(padding.left, y);
      ctx.lineTo(width - padding.right, y);
      ctx.stroke();
    }

    if (!sourceData) {
      // Draw placeholder
      ctx.fillStyle = 'rgba(255, 255, 255, 0.1)';
      ctx.font = '12px Inter, sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('Load an image to see histogram', width / 2, height / 2);
      return;
    }

    // Normalize and draw histograms
    const bins = sourceData.length;
    const barWidth = chartWidth / bins;

    // Find max value for scaling
    let maxVal = 0;
    for (let i = 0; i < bins; i++) {
      if (sourceData[i] > maxVal) maxVal = sourceData[i];
      if (resultData && resultData[i] > maxVal) maxVal = resultData[i];
    }

    // Apply log scale for better visualization
    const logScale = (val: number) => {
      if (val <= 0) return 0;
      return Math.log10(val + 1) / Math.log10(maxVal + 1);
    };

    // Draw source histogram (gray, behind)
    ctx.fillStyle = 'rgba(148, 163, 184, 0.3)';
    for (let i = 0; i < bins; i++) {
      const h = logScale(sourceData[i]) * chartHeight;
      const x = padding.left + i * barWidth;
      const y = padding.top + chartHeight - h;
      ctx.fillRect(x, y, barWidth, h);
    }

    // Draw source outline
    ctx.strokeStyle = 'rgba(148, 163, 184, 0.6)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let i = 0; i < bins; i++) {
      const h = logScale(sourceData[i]) * chartHeight;
      const x = padding.left + i * barWidth + barWidth / 2;
      const y = padding.top + chartHeight - h;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();

    // Draw result histogram (gradient fill)
    if (resultData) {
      const gradient = ctx.createLinearGradient(padding.left, 0, width - padding.right, 0);
      gradient.addColorStop(0, 'rgba(99, 102, 241, 0.4)');
      gradient.addColorStop(0.5, 'rgba(168, 85, 247, 0.4)');
      gradient.addColorStop(1, 'rgba(236, 72, 153, 0.4)');

      ctx.fillStyle = gradient;
      for (let i = 0; i < bins; i++) {
        const h = logScale(resultData[i]) * chartHeight;
        const x = padding.left + i * barWidth;
        const y = padding.top + chartHeight - h;
        ctx.fillRect(x, y, barWidth, h);
      }

      // Draw result outline
      const lineGradient = ctx.createLinearGradient(padding.left, 0, width - padding.right, 0);
      lineGradient.addColorStop(0, '#6366f1');
      lineGradient.addColorStop(0.5, '#a855f7');
      lineGradient.addColorStop(1, '#ec4899');

      ctx.strokeStyle = lineGradient;
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      for (let i = 0; i < bins; i++) {
        const h = logScale(resultData[i]) * chartHeight;
        const x = padding.left + i * barWidth + barWidth / 2;
        const y = padding.top + chartHeight - h;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    // Draw axis labels
    ctx.fillStyle = 'rgba(255, 255, 255, 0.4)';
    ctx.font = '10px Inter, sans-serif';
    ctx.textAlign = 'left';
    ctx.fillText('0', padding.left, height - 4);
    ctx.textAlign = 'right';
    ctx.fillText('1', width - padding.right, height - 4);
    ctx.textAlign = 'center';
    ctx.fillText('0.5', width / 2, height - 4);
  }, [sourceData, resultData]);

  return (
    <div className="histogram-panel">
      <div className="panel-header">
        <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
          <rect x="2" y="8" width="2" height="6" fill="currentColor" opacity="0.5" />
          <rect x="5" y="5" width="2" height="9" fill="currentColor" opacity="0.7" />
          <rect x="8" y="3" width="2" height="11" fill="currentColor" opacity="0.9" />
          <rect x="11" y="6" width="2" height="8" fill="currentColor" opacity="0.6" />
        </svg>
        <span>Histogram</span>
      </div>
      <div className="histogram-content">
        <canvas
          ref={canvasRef}
          width={300}
          height={120}
          className="histogram-canvas"
        />
        <div className="histogram-legend">
          <div className="legend-item">
            <span className="legend-color source" />
            <span className="legend-label">Source</span>
          </div>
          <div className="legend-item">
            <span className="legend-color result" />
            <span className="legend-label">Result</span>
          </div>
        </div>
      </div>

      <style>{`
        .histogram-panel {
          border-top: 1px solid var(--border-subtle);
        }

        .panel-header {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
          padding: var(--space-sm) var(--space-md);
          border-bottom: 1px solid var(--border-subtle);
          font-size: 12px;
          font-weight: 600;
          color: var(--text-secondary);
        }

        .histogram-content {
          padding: var(--space-md);
        }

        .histogram-canvas {
          width: 100%;
          height: auto;
          border-radius: var(--radius-sm);
          border: 1px solid var(--border-subtle);
        }

        .histogram-legend {
          display: flex;
          justify-content: center;
          gap: var(--space-lg);
          margin-top: var(--space-sm);
        }

        .legend-item {
          display: flex;
          align-items: center;
          gap: var(--space-xs);
        }

        .legend-color {
          width: 12px;
          height: 3px;
          border-radius: 2px;
        }

        .legend-color.source {
          background: rgba(148, 163, 184, 0.6);
        }

        .legend-color.result {
          background: linear-gradient(90deg, #6366f1, #a855f7, #ec4899);
        }

        .legend-label {
          font-size: 10px;
          color: var(--text-muted);
        }
      `}</style>
    </div>
  );
}
