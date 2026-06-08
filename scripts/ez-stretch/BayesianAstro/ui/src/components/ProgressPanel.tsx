/**
 * Progress panel showing execution status, ETA, and run button
 * Features: progress bar, elapsed/remaining time, stage indicators
 */

import { useState, useEffect, useRef } from 'react';
import { Play, Loader2, CheckCircle, XCircle, Clock, Zap } from 'lucide-react';

interface ProgressPanelProps {
  isProcessing: boolean;
  progress: number;
  status: string;
  onExecute: () => void;
  canExecute: boolean;
}

function formatTime(seconds: number): string {
  if (seconds < 60) return `${Math.round(seconds)}s`;
  if (seconds < 3600) {
    const mins = Math.floor(seconds / 60);
    const secs = Math.round(seconds % 60);
    return `${mins}m ${secs}s`;
  }
  const hours = Math.floor(seconds / 3600);
  const mins = Math.floor((seconds % 3600) / 60);
  return `${hours}h ${mins}m`;
}

function getStageFromStatus(status: string): string {
  const lower = status.toLowerCase();
  if (lower.includes('load') || lower.includes('read')) return 'loading';
  if (lower.includes('accumulat')) return 'accumulating';
  if (lower.includes('classif') || lower.includes('analyz')) return 'classifying';
  if (lower.includes('fus')) return 'fusing';
  if (lower.includes('writ') || lower.includes('sav')) return 'saving';
  if (lower.includes('complete') || lower.includes('done')) return 'complete';
  if (lower.includes('error') || lower.includes('fail')) return 'error';
  return 'processing';
}

const STAGES = [
  { id: 'loading', label: 'Load Frames' },
  { id: 'accumulating', label: 'Accumulate Statistics' },
  { id: 'classifying', label: 'Classify Distributions' },
  { id: 'fusing', label: 'Fuse Pixels' },
  { id: 'saving', label: 'Save Output' },
];

export function ProgressPanel({
  isProcessing,
  progress,
  status,
  onExecute,
  canExecute,
}: ProgressPanelProps) {
  const [startTime, setStartTime] = useState<number | null>(null);
  const [elapsedSeconds, setElapsedSeconds] = useState(0);
  const [lastProgress, setLastProgress] = useState(0);
  const [lastProgressTime, setLastProgressTime] = useState<number | null>(null);
  const [eta, setEta] = useState<number | null>(null);
  const intervalRef = useRef<number | null>(null);

  const currentStage = getStageFromStatus(status);
  const isComplete = progress >= 100 && !isProcessing;
  const isError = currentStage === 'error';

  // Track processing start/stop
  useEffect(() => {
    if (isProcessing && !startTime) {
      setStartTime(Date.now());
      setLastProgress(0);
      setLastProgressTime(Date.now());
      setEta(null);
    } else if (!isProcessing && startTime) {
      setStartTime(null);
    }
  }, [isProcessing, startTime]);

  // Update elapsed time
  useEffect(() => {
    if (isProcessing && startTime) {
      intervalRef.current = window.setInterval(() => {
        setElapsedSeconds((Date.now() - startTime) / 1000);
      }, 1000);
    } else {
      if (intervalRef.current) {
        clearInterval(intervalRef.current);
        intervalRef.current = null;
      }
    }

    return () => {
      if (intervalRef.current) {
        clearInterval(intervalRef.current);
      }
    };
  }, [isProcessing, startTime]);

  // Calculate ETA based on progress rate
  useEffect(() => {
    if (isProcessing && progress > lastProgress && progress < 100) {
      const now = Date.now();
      if (lastProgressTime && lastProgress > 0) {
        const progressDelta = progress - lastProgress;
        const timeDelta = (now - lastProgressTime) / 1000;
        const rate = progressDelta / timeDelta; // percent per second

        if (rate > 0) {
          const remaining = (100 - progress) / rate;
          setEta(remaining);
        }
      }

      setLastProgress(progress);
      setLastProgressTime(now);
    }
  }, [progress, isProcessing, lastProgress, lastProgressTime]);

  // Reset on completion
  useEffect(() => {
    if (!isProcessing) {
      setElapsedSeconds(0);
      setEta(null);
    }
  }, [isProcessing]);

  return (
    <div className="bg-gray-800 rounded-lg p-4">
      <div className="flex items-center justify-between mb-4">
        <h2 className="text-lg font-semibold">Execution</h2>
        {isProcessing && (
          <div className="flex items-center gap-1 text-blue-400 text-sm">
            <Zap size={14} />
            GPU
          </div>
        )}
      </div>

      {/* Stage indicators (only show during/after processing) */}
      {(isProcessing || isComplete || isError) && (
        <div className="mb-4">
          <div className="flex justify-between mb-2">
            {STAGES.map((stage, index) => {
              const stageIndex = STAGES.findIndex(s => s.id === currentStage);
              const isActive = stage.id === currentStage;
              const isPast = index < stageIndex || isComplete;

              return (
                <div
                  key={stage.id}
                  className={`flex flex-col items-center flex-1 ${
                    index < STAGES.length - 1 ? 'border-r border-gray-700' : ''
                  }`}
                >
                  <div
                    className={`w-3 h-3 rounded-full mb-1 ${
                      isPast
                        ? 'bg-green-500'
                        : isActive
                        ? 'bg-blue-500 animate-pulse'
                        : 'bg-gray-600'
                    }`}
                  />
                  <span
                    className={`text-xs text-center px-1 ${
                      isActive ? 'text-blue-400' : isPast ? 'text-green-400' : 'text-gray-500'
                    }`}
                  >
                    {stage.label}
                  </span>
                </div>
              );
            })}
          </div>
        </div>
      )}

      {/* Progress bar */}
      <div className="mb-4">
        <div className="flex justify-between text-sm mb-1">
          <span className={isError ? 'text-red-400' : ''}>{status}</span>
          <span>{Math.round(progress)}%</span>
        </div>
        <div className="w-full bg-gray-700 rounded-full h-3 overflow-hidden">
          <div
            className={`h-3 rounded-full transition-all duration-300 ${
              isError
                ? 'bg-red-500'
                : isComplete
                ? 'bg-green-500'
                : 'bg-blue-500'
            }`}
            style={{ width: `${progress}%` }}
          />
        </div>
      </div>

      {/* Time info */}
      {isProcessing && (
        <div className="flex justify-between text-sm text-gray-400 mb-4">
          <div className="flex items-center gap-1">
            <Clock size={14} />
            <span>Elapsed: {formatTime(elapsedSeconds)}</span>
          </div>
          {eta !== null && eta > 0 && (
            <span>ETA: ~{formatTime(eta)}</span>
          )}
        </div>
      )}

      {/* Completion message */}
      {isComplete && !isProcessing && (
        <div className="flex items-center gap-2 text-green-400 mb-4 p-2 bg-green-900/20 rounded">
          <CheckCircle size={18} />
          <span>Processing complete!</span>
        </div>
      )}

      {isError && (
        <div className="flex items-center gap-2 text-red-400 mb-4 p-2 bg-red-900/20 rounded">
          <XCircle size={18} />
          <span>Processing failed. Check the status message for details.</span>
        </div>
      )}

      {/* Execute button */}
      <button
        onClick={onExecute}
        disabled={!canExecute}
        className={`w-full flex items-center justify-center gap-2 py-3 rounded-lg font-semibold transition-all ${
          canExecute
            ? 'bg-green-600 hover:bg-green-700 text-white shadow-lg hover:shadow-green-900/50'
            : 'bg-gray-600 text-gray-400 cursor-not-allowed'
        }`}
      >
        {isProcessing ? (
          <>
            <Loader2 size={20} className="animate-spin" />
            Processing...
          </>
        ) : (
          <>
            <Play size={20} />
            Execute
          </>
        )}
      </button>

      {!canExecute && !isProcessing && (
        <p className="text-xs text-gray-400 mt-2 text-center">
          Add input files to enable execution
        </p>
      )}
    </div>
  );
}
