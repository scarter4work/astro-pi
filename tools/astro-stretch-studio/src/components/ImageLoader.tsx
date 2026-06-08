import React, { useCallback, useState, useRef } from 'react';

interface ImageLoaderProps {
  onImageLoad: (imageData: ImageData, fileName: string) => void;
}

export function ImageLoader({ onImageLoad }: ImageLoaderProps) {
  const [isDragging, setIsDragging] = useState(false);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);

  const loadImage = useCallback(async (file: File) => {
    setIsLoading(true);
    setError(null);

    try {
      const url = URL.createObjectURL(file);
      const img = new Image();

      await new Promise<void>((resolve, reject) => {
        img.onload = () => resolve();
        img.onerror = () => reject(new Error('Failed to load image'));
        img.src = url;
      });

      // Create canvas and get ImageData
      const canvas = document.createElement('canvas');
      canvas.width = img.width;
      canvas.height = img.height;

      const ctx = canvas.getContext('2d');
      if (!ctx) throw new Error('Could not get canvas context');

      ctx.drawImage(img, 0, 0);
      const imageData = ctx.getImageData(0, 0, img.width, img.height);

      URL.revokeObjectURL(url);
      onImageLoad(imageData, file.name);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to load image');
    } finally {
      setIsLoading(false);
    }
  }, [onImageLoad]);

  const handleDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(false);

    const file = e.dataTransfer.files[0];
    if (file && file.type.startsWith('image/')) {
      loadImage(file);
    } else {
      setError('Please drop an image file');
    }
  }, [loadImage]);

  const handleDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragging(true);
  }, []);

  const handleDragLeave = useCallback(() => {
    setIsDragging(false);
  }, []);

  const handleFileSelect = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (file) {
      loadImage(file);
    }
  }, [loadImage]);

  const handleClick = () => {
    fileInputRef.current?.click();
  };

  // Demo image generation
  const loadDemoImage = useCallback(() => {
    setIsLoading(true);
    setError(null);

    // Generate a synthetic nebula-like image
    const width = 800;
    const height = 600;
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d')!;

    // Black background
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, width, height);

    // Add noise and faint nebula patterns
    const imageData = ctx.getImageData(0, 0, width, height);
    const data = imageData.data;

    // Create nebula-like gradients
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const idx = (y * width + x) * 4;

        // Distance from center
        const dx = (x - width / 2) / width;
        const dy = (y - height / 2) / height;
        const dist = Math.sqrt(dx * dx + dy * dy);

        // Multiple noise patterns
        const noise1 = (Math.sin(x * 0.02) + Math.sin(y * 0.02) + Math.sin((x + y) * 0.015)) / 6 + 0.5;
        const noise2 = (Math.sin(x * 0.05 + y * 0.03) + Math.sin(x * 0.03 - y * 0.04)) / 4 + 0.5;

        // Nebula core
        const core = Math.exp(-dist * dist * 8) * 0.4;

        // Nebula halo
        const halo = Math.exp(-dist * dist * 3) * noise1 * 0.25;

        // Faint outer structure
        const outer = Math.exp(-dist * dist * 1.5) * noise2 * 0.15;

        // Add some random noise
        const randomNoise = Math.random() * 0.02;

        // Combine
        let r = (core * 0.8 + halo * 0.6 + outer * 0.3 + randomNoise) * 0.3;
        let g = (core * 0.5 + halo * 0.8 + outer * 0.5 + randomNoise) * 0.2;
        let b = (core * 0.3 + halo * 0.4 + outer * 0.7 + randomNoise) * 0.25;

        // Add some stars
        if (Math.random() > 0.9995) {
          const starBright = Math.random() * 0.5 + 0.5;
          r += starBright;
          g += starBright;
          b += starBright * 0.9;
        }

        data[idx] = Math.min(255, r * 255);
        data[idx + 1] = Math.min(255, g * 255);
        data[idx + 2] = Math.min(255, b * 255);
        data[idx + 3] = 255;
      }
    }

    onImageLoad(imageData, 'demo_nebula.png');
    setIsLoading(false);
  }, [onImageLoad]);

  return (
    <div className="image-loader">
      <input
        ref={fileInputRef}
        type="file"
        accept="image/*"
        onChange={handleFileSelect}
        style={{ display: 'none' }}
      />

      <div
        className={`drop-zone ${isDragging ? 'dragging' : ''} ${isLoading ? 'loading' : ''}`}
        onDrop={handleDrop}
        onDragOver={handleDragOver}
        onDragLeave={handleDragLeave}
        onClick={handleClick}
      >
        {isLoading ? (
          <div className="loading-state">
            <div className="loader-spinner" />
            <p>Loading image...</p>
          </div>
        ) : (
          <>
            <div className="drop-icon">
              <svg width="64" height="64" viewBox="0 0 64 64" fill="none">
                <rect x="8" y="12" width="48" height="40" rx="4" stroke="currentColor" strokeWidth="2" fill="none" />
                <circle cx="22" cy="28" r="6" stroke="currentColor" strokeWidth="2" fill="none" />
                <path d="M8 44 L24 32 L36 44 L48 28 L56 36" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" fill="none" />
              </svg>
            </div>
            <h3 className="drop-title">Drop your image here</h3>
            <p className="drop-subtitle">or click to browse files</p>
            <p className="drop-hint">Supports PNG, JPEG, TIFF, and other common formats</p>
          </>
        )}
      </div>

      {error && (
        <div className="error-message">
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
            <circle cx="8" cy="8" r="6" stroke="currentColor" strokeWidth="1.5" fill="none" />
            <path d="M8 5 L8 9 M8 11 L8 11.5" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
          </svg>
          {error}
        </div>
      )}

      <button className="demo-btn" onClick={loadDemoImage}>
        <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
          <path d="M8 2 L8 14 M2 8 L14 8" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
          <circle cx="8" cy="8" r="6" stroke="currentColor" strokeWidth="1.5" fill="none" strokeDasharray="2 2" />
        </svg>
        Load Demo Nebula Image
      </button>

      <style>{`
        .image-loader {
          display: flex;
          flex-direction: column;
          align-items: center;
          justify-content: center;
          height: 100%;
          padding: var(--space-xl);
        }

        .drop-zone {
          display: flex;
          flex-direction: column;
          align-items: center;
          justify-content: center;
          width: 100%;
          max-width: 500px;
          padding: var(--space-xl) var(--space-lg);
          border: 2px dashed var(--border-strong);
          border-radius: var(--radius-lg);
          background: var(--bg-secondary);
          cursor: pointer;
          transition: all var(--transition-normal);
        }

        .drop-zone:hover {
          border-color: var(--accent-primary);
          background: var(--bg-tertiary);
        }

        .drop-zone.dragging {
          border-color: var(--accent-primary);
          background: rgba(99, 102, 241, 0.1);
          transform: scale(1.02);
        }

        .drop-zone.loading {
          pointer-events: none;
          opacity: 0.8;
        }

        .drop-icon {
          color: var(--text-muted);
          margin-bottom: var(--space-md);
          transition: all var(--transition-normal);
        }

        .drop-zone:hover .drop-icon {
          color: var(--accent-primary);
          transform: translateY(-4px);
        }

        .drop-title {
          font-size: 18px;
          font-weight: 600;
          color: var(--text-primary);
          margin-bottom: var(--space-xs);
        }

        .drop-subtitle {
          color: var(--text-secondary);
          margin-bottom: var(--space-md);
        }

        .drop-hint {
          font-size: 12px;
          color: var(--text-muted);
        }

        .loading-state {
          display: flex;
          flex-direction: column;
          align-items: center;
          gap: var(--space-md);
        }

        .loader-spinner {
          width: 48px;
          height: 48px;
          border: 3px solid var(--bg-hover);
          border-top-color: var(--accent-primary);
          border-radius: 50%;
          animation: spin 1s linear infinite;
        }

        .error-message {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
          margin-top: var(--space-md);
          padding: var(--space-sm) var(--space-md);
          background: rgba(239, 68, 68, 0.1);
          border: 1px solid rgba(239, 68, 68, 0.3);
          border-radius: var(--radius-sm);
          color: var(--error);
          font-size: 13px;
        }

        .demo-btn {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
          margin-top: var(--space-lg);
          padding: var(--space-sm) var(--space-lg);
          background: transparent;
          border: 1px solid var(--border-subtle);
          border-radius: var(--radius-sm);
          color: var(--text-muted);
          font-size: 13px;
          cursor: pointer;
          transition: all var(--transition-fast);
        }

        .demo-btn:hover {
          background: var(--bg-tertiary);
          border-color: var(--accent-primary);
          color: var(--text-primary);
        }

        @keyframes spin {
          to { transform: rotate(360deg); }
        }
      `}</style>
    </div>
  );
}
