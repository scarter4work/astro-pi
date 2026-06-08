import { AlgorithmType } from '../App';

interface HeaderProps {
  algorithm: AlgorithmType;
  onAlgorithmChange: (algo: AlgorithmType) => void;
  onExport: () => void;
  hasImage: boolean;
  isProcessing: boolean;
}

export function Header({
  algorithm,
  onAlgorithmChange,
  onExport,
  hasImage,
  isProcessing
}: HeaderProps) {
  return (
    <header className="header">
      <div className="header-left">
        <div className="logo">
          <svg width="28" height="28" viewBox="0 0 28 28" fill="none">
            <circle cx="14" cy="14" r="12" stroke="url(#logoGradient)" strokeWidth="2" />
            <circle cx="14" cy="14" r="6" fill="url(#logoGradient)" opacity="0.8" />
            <circle cx="14" cy="14" r="2" fill="white" />
            <defs>
              <linearGradient id="logoGradient" x1="0" y1="0" x2="28" y2="28">
                <stop stopColor="#818cf8" />
                <stop offset="1" stopColor="#ec4899" />
              </linearGradient>
            </defs>
          </svg>
          <span className="logo-text">AstroStretch Studio</span>
        </div>

        <div className="algorithm-tabs">
          <button
            className={`tab ${algorithm === 'ots' ? 'active' : ''}`}
            onClick={() => onAlgorithmChange('ots')}
          >
            <span className="tab-icon">
              <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
                <path d="M2 12 L6 8 L10 10 L14 4" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
                <circle cx="6" cy="8" r="1.5" fill="currentColor" />
                <circle cx="10" cy="10" r="1.5" fill="currentColor" />
              </svg>
            </span>
            <span className="tab-label">Optimal Transport</span>
            <span className="tab-abbr">OTS</span>
          </button>
          <button
            className={`tab ${algorithm === 'sas' ? 'active' : ''}`}
            onClick={() => onAlgorithmChange('sas')}
          >
            <span className="tab-icon">
              <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
                <path d="M1 8 Q4 4, 8 8 T15 8" stroke="currentColor" strokeWidth="1.5" fill="none" />
                <path d="M1 8 Q4 12, 8 8 T15 8" stroke="currentColor" strokeWidth="1.5" fill="none" opacity="0.5" />
              </svg>
            </span>
            <span className="tab-label">Starlet Arctan</span>
            <span className="tab-abbr">SAS</span>
          </button>
        </div>
      </div>

      <div className="header-right">
        {isProcessing && (
          <div className="processing-indicator">
            <svg className="spinner" width="16" height="16" viewBox="0 0 16 16">
              <circle cx="8" cy="8" r="6" stroke="currentColor" strokeWidth="2" fill="none" strokeDasharray="28" strokeLinecap="round" />
            </svg>
            <span>Processing...</span>
          </div>
        )}

        <button
          className="btn btn-primary"
          onClick={onExport}
          disabled={!hasImage}
        >
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
            <path d="M8 2 L8 10 M4 6 L8 2 L12 6" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
            <path d="M3 10 L3 13 L13 13 L13 10" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
          </svg>
          Export
        </button>
      </div>

      <style>{`
        .header {
          display: flex;
          align-items: center;
          justify-content: space-between;
          height: 56px;
          padding: 0 var(--space-lg);
          background: var(--bg-secondary);
          border-bottom: 1px solid var(--border-subtle);
        }

        .header-left {
          display: flex;
          align-items: center;
          gap: var(--space-xl);
        }

        .logo {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
        }

        .logo-text {
          font-size: 16px;
          font-weight: 600;
          background: linear-gradient(135deg, #818cf8 0%, #ec4899 100%);
          -webkit-background-clip: text;
          -webkit-text-fill-color: transparent;
          background-clip: text;
        }

        .algorithm-tabs {
          display: flex;
          gap: var(--space-xs);
          background: var(--bg-tertiary);
          padding: 4px;
          border-radius: var(--radius-md);
        }

        .tab {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
          padding: var(--space-sm) var(--space-md);
          background: transparent;
          border: none;
          border-radius: var(--radius-sm);
          color: var(--text-muted);
          font-size: 13px;
          font-weight: 500;
          cursor: pointer;
          transition: all var(--transition-fast);
        }

        .tab:hover {
          color: var(--text-secondary);
          background: var(--bg-hover);
        }

        .tab.active {
          background: var(--accent-primary);
          color: white;
        }

        .tab-icon {
          display: flex;
          align-items: center;
        }

        .tab-abbr {
          display: none;
          font-size: 11px;
          font-weight: 600;
          opacity: 0.8;
        }

        @media (max-width: 900px) {
          .tab-label { display: none; }
          .tab-abbr { display: inline; }
        }

        .header-right {
          display: flex;
          align-items: center;
          gap: var(--space-md);
        }

        .processing-indicator {
          display: flex;
          align-items: center;
          gap: var(--space-sm);
          color: var(--text-muted);
          font-size: 12px;
        }

        .spinner {
          animation: spin 1s linear infinite;
          color: var(--accent-primary);
        }

        @keyframes spin {
          from { transform: rotate(0deg); }
          to { transform: rotate(360deg); }
        }
      `}</style>
    </header>
  );
}
