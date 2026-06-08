/**
 * File list component for input frame selection
 * Features: drag-drop, file info display, sorting, keyboard navigation
 */

import { useState, useCallback, useRef, useEffect } from 'react';
import { Trash2, FolderOpen, X, FileImage, ArrowUpDown, ChevronUp, ChevronDown } from 'lucide-react';

interface FileInfo {
  path: string;
  name: string;
  size?: number;
}

interface FileListProps {
  files: string[];
  onAddFiles: (paths: string[]) => void;
  onRemoveFile: (index: number) => void;
  onClearFiles: () => void;
  disabled?: boolean;
}

type SortField = 'name' | 'none';
type SortDirection = 'asc' | 'desc';

function formatFileSize(bytes?: number): string {
  if (!bytes) return '';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function getFileName(path: string): string {
  return path.split(/[/\\]/).pop() || path;
}

export function FileList({ files, onAddFiles, onRemoveFile, onClearFiles, disabled }: FileListProps) {
  const [isDragOver, setIsDragOver] = useState(false);
  const [selectedIndex, setSelectedIndex] = useState<number | null>(null);
  const [sortField, setSortField] = useState<SortField>('none');
  const [sortDirection, setSortDirection] = useState<SortDirection>('asc');
  const listRef = useRef<HTMLUListElement>(null);

  // Convert files to FileInfo objects
  const fileInfos: FileInfo[] = files.map(path => ({
    path,
    name: getFileName(path),
  }));

  // Sort files if needed
  const sortedFiles = [...fileInfos];
  if (sortField !== 'none') {
    sortedFiles.sort((a, b) => {
      const cmp = a.name.localeCompare(b.name);
      return sortDirection === 'asc' ? cmp : -cmp;
    });
  }

  const handleDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragOver(false);
    if (disabled) return;

    const paths: string[] = [];
    for (const file of Array.from(e.dataTransfer.files)) {
      const name = file.name.toLowerCase();
      if (name.endsWith('.fits') || name.endsWith('.fit') || name.endsWith('.fts')) {
        const filePath = (file as unknown as { path?: string }).path || file.name;
        paths.push(filePath);
      }
    }
    if (paths.length > 0) {
      onAddFiles(paths);
    }
  }, [disabled, onAddFiles]);

  const handleDragOver = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    if (!disabled) {
      setIsDragOver(true);
    }
  }, [disabled]);

  const handleDragLeave = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    setIsDragOver(false);
  }, []);

  const toggleSort = useCallback(() => {
    if (sortField === 'none') {
      setSortField('name');
      setSortDirection('asc');
    } else if (sortDirection === 'asc') {
      setSortDirection('desc');
    } else {
      setSortField('none');
    }
  }, [sortField, sortDirection]);

  // Keyboard navigation
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (disabled || files.length === 0) return;

      switch (e.key) {
        case 'ArrowUp':
          e.preventDefault();
          setSelectedIndex(prev => {
            if (prev === null || prev === 0) return files.length - 1;
            return prev - 1;
          });
          break;
        case 'ArrowDown':
          e.preventDefault();
          setSelectedIndex(prev => {
            if (prev === null || prev === files.length - 1) return 0;
            return prev + 1;
          });
          break;
        case 'Delete':
        case 'Backspace':
          if (selectedIndex !== null) {
            e.preventDefault();
            const indexToRemove = selectedIndex;
            // Find the original index if sorted
            const originalIndex = files.indexOf(sortedFiles[indexToRemove].path);
            onRemoveFile(originalIndex);
            // Adjust selection
            if (selectedIndex >= files.length - 1) {
              setSelectedIndex(Math.max(0, files.length - 2));
            }
          }
          break;
        case 'Escape':
          setSelectedIndex(null);
          break;
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [disabled, files, selectedIndex, sortedFiles, onRemoveFile]);

  // Scroll selected item into view
  useEffect(() => {
    if (selectedIndex !== null && listRef.current) {
      const items = listRef.current.querySelectorAll('li');
      items[selectedIndex]?.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    }
  }, [selectedIndex]);

  return (
    <div className="bg-gray-800 rounded-lg p-4">
      {/* Header */}
      <div className="flex justify-between items-center mb-3">
        <div className="flex items-center gap-3">
          <h2 className="text-lg font-semibold">Input Frames</h2>
          <span className="text-sm text-gray-400 bg-gray-700 px-2 py-0.5 rounded">
            {files.length} file{files.length !== 1 ? 's' : ''}
          </span>
        </div>
        <div className="flex gap-2">
          <button
            onClick={toggleSort}
            disabled={disabled || files.length < 2}
            className="flex items-center gap-1 px-3 py-1 bg-gray-700 hover:bg-gray-600 disabled:bg-gray-700 disabled:opacity-50 disabled:cursor-not-allowed rounded text-sm"
            title="Sort files"
          >
            {sortField === 'none' ? (
              <ArrowUpDown size={16} />
            ) : sortDirection === 'asc' ? (
              <ChevronUp size={16} />
            ) : (
              <ChevronDown size={16} />
            )}
            Sort
          </button>
          <button
            onClick={() => {/* TODO: Open file dialog via bridge */}}
            disabled={disabled}
            className="flex items-center gap-1 px-3 py-1 bg-blue-600 hover:bg-blue-700 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm transition-colors"
          >
            <FolderOpen size={16} />
            Add Files
          </button>
          <button
            onClick={onClearFiles}
            disabled={disabled || files.length === 0}
            className="flex items-center gap-1 px-3 py-1 bg-red-600 hover:bg-red-700 disabled:bg-gray-600 disabled:cursor-not-allowed rounded text-sm transition-colors"
          >
            <Trash2 size={16} />
            Clear
          </button>
        </div>
      </div>

      {/* Keyboard hint */}
      {files.length > 0 && (
        <p className="text-xs text-gray-500 mb-2">
          Tip: Use arrow keys to navigate, Delete to remove selected file
        </p>
      )}

      {/* Drop zone */}
      <div
        onDrop={handleDrop}
        onDragOver={handleDragOver}
        onDragLeave={handleDragLeave}
        className={`min-h-[300px] max-h-[500px] overflow-y-auto border-2 border-dashed rounded-lg transition-all ${
          disabled
            ? 'border-gray-700 bg-gray-900'
            : isDragOver
            ? 'border-blue-400 bg-blue-900/20'
            : 'border-gray-600 hover:border-gray-500'
        }`}
      >
        {files.length === 0 ? (
          <div className="h-[300px] flex flex-col items-center justify-center text-gray-500 p-4">
            <FileImage size={48} className="mb-3 opacity-50" />
            <p className="text-center">Drag and drop FITS files here</p>
            <p className="text-sm mt-1">or click "Add Files" to browse</p>
            <p className="text-xs mt-3 text-gray-600">Supported: .fits, .fit, .fts</p>
          </div>
        ) : (
          <ul ref={listRef} className="p-2 space-y-1">
            {sortedFiles.map((file, displayIndex) => {
              const originalIndex = files.indexOf(file.path);
              return (
                <li
                  key={file.path}
                  onClick={() => setSelectedIndex(displayIndex)}
                  className={`flex items-center justify-between rounded px-3 py-2 text-sm cursor-pointer transition-colors ${
                    selectedIndex === displayIndex
                      ? 'bg-blue-600/30 border border-blue-500'
                      : 'bg-gray-700 hover:bg-gray-650 border border-transparent'
                  }`}
                >
                  <div className="flex items-center gap-2 flex-1 min-w-0">
                    <FileImage size={16} className="text-gray-400 flex-shrink-0" />
                    <div className="flex-1 min-w-0">
                      <span className="truncate block" title={file.path}>
                        {file.name}
                      </span>
                      {file.size && (
                        <span className="text-xs text-gray-500">
                          {formatFileSize(file.size)}
                        </span>
                      )}
                    </div>
                  </div>
                  <div className="flex items-center gap-2 ml-2">
                    <span className="text-xs text-gray-500">#{originalIndex + 1}</span>
                    <button
                      onClick={(e) => {
                        e.stopPropagation();
                        onRemoveFile(originalIndex);
                      }}
                      disabled={disabled}
                      className="text-gray-400 hover:text-red-400 disabled:opacity-50 p-1 rounded hover:bg-gray-600 transition-colors"
                      title="Remove file"
                    >
                      <X size={14} />
                    </button>
                  </div>
                </li>
              );
            })}
          </ul>
        )}
      </div>

      {/* Summary footer */}
      {files.length > 0 && (
        <div className="mt-3 pt-3 border-t border-gray-700 flex justify-between text-sm text-gray-400">
          <span>Ready to process {files.length} frame{files.length !== 1 ? 's' : ''}</span>
          {sortField !== 'none' && (
            <span>Sorted by name ({sortDirection === 'asc' ? 'A-Z' : 'Z-A'})</span>
          )}
        </div>
      )}
    </div>
  );
}
