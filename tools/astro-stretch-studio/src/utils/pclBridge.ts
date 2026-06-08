/**
 * PCL Bridge - Communication layer between WebView and PixInsight
 *
 * This module provides bidirectional communication with PixInsight's
 * PCL framework through the WebView interface.
 */

// Type definitions for PixInsight WebView communication
declare global {
  interface Window {
    // PixInsight provides these functions for WebView communication
    pclSendMessage?: (message: string) => void;
    pclOnMessage?: (callback: (message: string) => void) => void;
    pclGetImageData?: () => Promise<ImageData>;
    pclSetImageData?: (data: ImageData) => void;
    pclGetActiveViewId?: () => string | null;
    pclExecuteScript?: (script: string) => Promise<unknown>;
  }
}

export interface PCLMessage {
  type: string;
  payload?: unknown;
  id?: string;
}

export interface ImageInfo {
  id: string;
  width: number;
  height: number;
  numberOfChannels: number;
  bitsPerSample: number;
  isColor: boolean;
}

class PCLBridge {
  private messageHandlers: Map<string, (payload: unknown) => void> = new Map();
  private pendingRequests: Map<string, { resolve: (value: unknown) => void; reject: (reason: unknown) => void }> = new Map();
  private requestId = 0;
  private isConnected = false;

  constructor() {
    this.setupMessageListener();
    this.checkConnection();
  }

  private checkConnection() {
    // Check if running inside PixInsight WebView
    this.isConnected = typeof window.pclSendMessage === 'function';

    if (this.isConnected) {
      console.log('[PCL Bridge] Connected to PixInsight');
      this.sendMessage({ type: 'ready' });
    } else {
      console.log('[PCL Bridge] Running in standalone mode (no PixInsight connection)');
    }
  }

  private setupMessageListener() {
    if (typeof window.pclOnMessage === 'function') {
      window.pclOnMessage((messageStr: string) => {
        try {
          const message: PCLMessage = JSON.parse(messageStr);
          this.handleMessage(message);
        } catch (e) {
          console.error('[PCL Bridge] Failed to parse message:', e);
        }
      });
    }

    // Fallback: Listen for postMessage events (for testing)
    window.addEventListener('message', (event) => {
      if (event.data && typeof event.data === 'object' && event.data.type) {
        this.handleMessage(event.data as PCLMessage);
      }
    });
  }

  private handleMessage(message: PCLMessage) {
    // Check if this is a response to a pending request
    if (message.id && this.pendingRequests.has(message.id)) {
      const { resolve, reject } = this.pendingRequests.get(message.id)!;
      this.pendingRequests.delete(message.id);

      if (message.type === 'error') {
        reject(message.payload);
      } else {
        resolve(message.payload);
      }
      return;
    }

    // Handle by message type
    const handler = this.messageHandlers.get(message.type);
    if (handler) {
      handler(message.payload);
    } else {
      console.log('[PCL Bridge] Unhandled message type:', message.type);
    }
  }

  /**
   * Send a message to PixInsight
   */
  sendMessage(message: PCLMessage): void {
    if (window.pclSendMessage) {
      window.pclSendMessage(JSON.stringify(message));
    } else {
      // Fallback for testing
      console.log('[PCL Bridge] Would send message:', message);
    }
  }

  /**
   * Send a request and wait for response
   */
  async request<T>(type: string, payload?: unknown): Promise<T> {
    return new Promise((resolve, reject) => {
      const id = `req_${++this.requestId}`;
      this.pendingRequests.set(id, { resolve: resolve as (value: unknown) => void, reject });

      // Timeout after 30 seconds
      setTimeout(() => {
        if (this.pendingRequests.has(id)) {
          this.pendingRequests.delete(id);
          reject(new Error('Request timeout'));
        }
      }, 30000);

      this.sendMessage({ type, payload, id });
    });
  }

  /**
   * Register a handler for a message type
   */
  onMessage(type: string, handler: (payload: unknown) => void): void {
    this.messageHandlers.set(type, handler);
  }

  /**
   * Remove a message handler
   */
  offMessage(type: string): void {
    this.messageHandlers.delete(type);
  }

  /**
   * Check if connected to PixInsight
   */
  get connected(): boolean {
    return this.isConnected;
  }

  // ========== PixInsight-specific Methods ==========

  /**
   * Get list of open images in PixInsight
   */
  async getOpenImages(): Promise<ImageInfo[]> {
    if (!this.isConnected) {
      console.log('[PCL Bridge] Not connected, returning empty list');
      return [];
    }
    return this.request<ImageInfo[]>('getOpenImages');
  }

  /**
   * Get ImageData from the active view
   */
  async getActiveImageData(): Promise<ImageData | null> {
    if (window.pclGetImageData) {
      return window.pclGetImageData();
    }
    if (!this.isConnected) return null;
    return this.request<ImageData>('getActiveImageData');
  }

  /**
   * Get the ID of the active view
   */
  getActiveViewId(): string | null {
    if (window.pclGetActiveViewId) {
      return window.pclGetActiveViewId();
    }
    return null;
  }

  /**
   * Send processed ImageData back to PixInsight
   */
  async setImageData(data: ImageData, viewId?: string): Promise<void> {
    if (window.pclSetImageData) {
      window.pclSetImageData(data);
      return;
    }
    if (!this.isConnected) {
      console.log('[PCL Bridge] Not connected, cannot set image data');
      return;
    }
    await this.request('setImageData', { data, viewId });
  }

  /**
   * Execute a PixScript command
   */
  async executeScript(script: string): Promise<unknown> {
    if (window.pclExecuteScript) {
      return window.pclExecuteScript(script);
    }
    if (!this.isConnected) {
      console.log('[PCL Bridge] Not connected, cannot execute script');
      return null;
    }
    return this.request('executeScript', { script });
  }

  /**
   * Create a new image window in PixInsight
   */
  async createImageWindow(
    id: string,
    data: ImageData,
    options?: {
      copyMetadataFrom?: string;
    }
  ): Promise<string | null> {
    if (!this.isConnected) {
      console.log('[PCL Bridge] Not connected, cannot create image window');
      return null;
    }
    return this.request<string>('createImageWindow', { id, data, ...options });
  }

  /**
   * Apply a process to the active view (for undo history)
   */
  async applyProcess(
    processName: string,
    parameters: Record<string, unknown>
  ): Promise<void> {
    if (!this.isConnected) {
      console.log('[PCL Bridge] Not connected, cannot apply process');
      return;
    }
    await this.request('applyProcess', { processName, parameters });
  }

  /**
   * Log a message to PixInsight's console
   */
  log(message: string, level: 'info' | 'warning' | 'error' = 'info'): void {
    this.sendMessage({
      type: 'log',
      payload: { message, level }
    });
  }
}

// Singleton instance
export const pclBridge = new PCLBridge();

// React hook for PCL Bridge
import { useState, useEffect } from 'react';

export function usePCLBridge() {
  const [connected, setConnected] = useState(pclBridge.connected);

  useEffect(() => {
    // Re-check connection status
    setConnected(pclBridge.connected);

    // Listen for connection changes
    const handler = (payload: unknown) => {
      if (payload && typeof payload === 'object' && 'connected' in payload) {
        setConnected((payload as { connected: boolean }).connected);
      }
    };

    pclBridge.onMessage('connectionStatus', handler);
    return () => pclBridge.offMessage('connectionStatus');
  }, []);

  return {
    connected,
    bridge: pclBridge
  };
}
