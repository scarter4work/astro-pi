/**
 * React hook for Qt WebChannel bridge connection
 */

import { useEffect, useState, useCallback } from 'react';
import type { BayesianAstroBridge, ProcessingState } from '../types/bridge';

interface BridgeState {
  connected: boolean;
  bridge: BayesianAstroBridge | null;
  fusionStrategy: number;
  outlierSigma: number;
  confidenceThreshold: number;
  useGPU: boolean;
  generateConfidenceMap: boolean;
  files: string[];
  processing: ProcessingState;
}

export function useBridge() {
  const [state, setState] = useState<BridgeState>({
    connected: false,
    bridge: null,
    fusionStrategy: 1, // ConfidenceWeighted
    outlierSigma: 3.0,
    confidenceThreshold: 0.1,
    useGPU: true,
    generateConfidenceMap: true,
    files: [],
    processing: {
      isProcessing: false,
      progress: 0,
      status: 'Ready',
    },
  });

  useEffect(() => {
    // Check if running in Qt WebEngine
    if (window.qt?.webChannelTransport && window.QWebChannel) {
      new window.QWebChannel(window.qt.webChannelTransport, (channel) => {
        const bridge = channel.objects.bayesianAstro;

        // Connect signal handlers
        bridge.fusionStrategyChanged.connect(() => {
          setState((s) => ({ ...s, fusionStrategy: bridge.fusionStrategy }));
        });

        bridge.outlierSigmaChanged.connect(() => {
          setState((s) => ({ ...s, outlierSigma: bridge.outlierSigma }));
        });

        bridge.confidenceThresholdChanged.connect(() => {
          setState((s) => ({ ...s, confidenceThreshold: bridge.confidenceThreshold }));
        });

        bridge.useGPUChanged.connect(() => {
          setState((s) => ({ ...s, useGPU: bridge.useGPU }));
        });

        bridge.generateConfidenceMapChanged.connect(() => {
          setState((s) => ({ ...s, generateConfidenceMap: bridge.generateConfidenceMap }));
        });

        bridge.filesChanged.connect(() => {
          setState((s) => ({ ...s, files: bridge.getFiles() }));
        });

        bridge.progressUpdated.connect((percent: number, status: string) => {
          setState((s) => ({
            ...s,
            processing: { ...s.processing, progress: percent, status },
          }));
        });

        bridge.executionComplete.connect((success: boolean, message: string) => {
          setState((s) => ({
            ...s,
            processing: {
              isProcessing: false,
              progress: success ? 100 : s.processing.progress,
              status: message,
            },
          }));
        });

        // Initial sync
        setState((s) => ({
          ...s,
          connected: true,
          bridge,
          fusionStrategy: bridge.fusionStrategy,
          outlierSigma: bridge.outlierSigma,
          confidenceThreshold: bridge.confidenceThreshold,
          useGPU: bridge.useGPU,
          generateConfidenceMap: bridge.generateConfidenceMap,
          files: bridge.getFiles(),
        }));
      });
    } else {
      // Development mode - mock bridge
      console.log('Running in development mode without Qt bridge');
      setState((s) => ({ ...s, connected: true }));
    }
  }, []);

  // Actions
  const setFusionStrategy = useCallback(
    (value: number) => {
      if (state.bridge) {
        state.bridge.fusionStrategy = value;
      }
      setState((s) => ({ ...s, fusionStrategy: value }));
    },
    [state.bridge]
  );

  const setOutlierSigma = useCallback(
    (value: number) => {
      if (state.bridge) {
        state.bridge.outlierSigma = value;
      }
      setState((s) => ({ ...s, outlierSigma: value }));
    },
    [state.bridge]
  );

  const setConfidenceThreshold = useCallback(
    (value: number) => {
      if (state.bridge) {
        state.bridge.confidenceThreshold = value;
      }
      setState((s) => ({ ...s, confidenceThreshold: value }));
    },
    [state.bridge]
  );

  const setUseGPU = useCallback(
    (value: boolean) => {
      if (state.bridge) {
        state.bridge.useGPU = value;
      }
      setState((s) => ({ ...s, useGPU: value }));
    },
    [state.bridge]
  );

  const setGenerateConfidenceMap = useCallback(
    (value: boolean) => {
      if (state.bridge) {
        state.bridge.generateConfidenceMap = value;
      }
      setState((s) => ({ ...s, generateConfidenceMap: value }));
    },
    [state.bridge]
  );

  const addFiles = useCallback(
    (paths: string[]) => {
      if (state.bridge) {
        state.bridge.addFiles(paths);
      } else {
        setState((s) => ({ ...s, files: [...s.files, ...paths] }));
      }
    },
    [state.bridge]
  );

  const removeFile = useCallback(
    (index: number) => {
      if (state.bridge) {
        state.bridge.removeFile(index);
      } else {
        setState((s) => ({
          ...s,
          files: s.files.filter((_, i) => i !== index),
        }));
      }
    },
    [state.bridge]
  );

  const clearFiles = useCallback(() => {
    if (state.bridge) {
      state.bridge.clearFiles();
    } else {
      setState((s) => ({ ...s, files: [] }));
    }
  }, [state.bridge]);

  const execute = useCallback(() => {
    setState((s) => ({
      ...s,
      processing: { isProcessing: true, progress: 0, status: 'Starting...' },
    }));

    if (state.bridge) {
      state.bridge.execute();
    } else {
      // Mock execution for development
      let progress = 0;
      const interval = setInterval(() => {
        progress += 10;
        setState((s) => ({
          ...s,
          processing: {
            isProcessing: progress < 100,
            progress,
            status: progress < 100 ? `Processing... ${progress}%` : 'Complete',
          },
        }));
        if (progress >= 100) clearInterval(interval);
      }, 500);
    }
  }, [state.bridge]);

  return {
    connected: state.connected,
    fusionStrategy: state.fusionStrategy,
    outlierSigma: state.outlierSigma,
    confidenceThreshold: state.confidenceThreshold,
    useGPU: state.useGPU,
    generateConfidenceMap: state.generateConfidenceMap,
    files: state.files,
    processing: state.processing,
    setFusionStrategy,
    setOutlierSigma,
    setConfidenceThreshold,
    setUseGPU,
    setGenerateConfidenceMap,
    addFiles,
    removeFile,
    clearFiles,
    execute,
  };
}
