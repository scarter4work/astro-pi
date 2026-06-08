/**
 * TypeScript types for Qt WebChannel bridge to C++ backend
 */

export enum FusionStrategy {
  MLE = 0,
  ConfidenceWeighted = 1,
  Lucky = 2,
  MultiScale = 3,
}

export interface BayesianAstroBridge {
  // Properties (reactive via Qt signals)
  fusionStrategy: number;
  outlierSigma: number;
  confidenceThreshold: number;
  useGPU: boolean;
  generateConfidenceMap: boolean;

  // Methods
  addFiles(paths: string[]): void;
  removeFile(index: number): void;
  clearFiles(): void;
  getFiles(): string[];
  execute(): void;
  setOutputDirectory(path: string): void;
  setOutputPrefix(prefix: string): void;

  // Signal connections
  fusionStrategyChanged: { connect: (callback: () => void) => void };
  outlierSigmaChanged: { connect: (callback: () => void) => void };
  confidenceThresholdChanged: { connect: (callback: () => void) => void };
  useGPUChanged: { connect: (callback: () => void) => void };
  generateConfidenceMapChanged: { connect: (callback: () => void) => void };
  filesChanged: { connect: (callback: () => void) => void };
  progressUpdated: { connect: (callback: (percent: number, status: string) => void) => void };
  executionComplete: { connect: (callback: (success: boolean, message: string) => void) => void };
}

declare global {
  interface Window {
    qt?: {
      webChannelTransport: unknown;
    };
    QWebChannel?: new (
      transport: unknown,
      callback: (channel: { objects: { bayesianAstro: BayesianAstroBridge } }) => void
    ) => void;
  }
}

export interface ProcessingState {
  isProcessing: boolean;
  progress: number;
  status: string;
}
