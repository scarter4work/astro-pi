# /pcl-template Command

Get a code template for common PCL development patterns.

## Usage

```
/pcl-template <templateName>
```

## Available Templates

### moduleDefinition
Basic PixInsight module structure with version macros and MetaModule class.

### processInterface
Complete ProcessInterface implementation with GUI controls and event handlers.

### imageProcessing
Image processing code with pixel access and progress monitoring.

### dialogTemplate
Custom dialog with sizers, controls, and event handlers.

### fileIO
XISF file reading and writing with error handling.

### threadedProcessing
Parallel image processing using Thread class.

## Examples

```
/pcl-template moduleDefinition
/pcl-template imageProcessing
/pcl-template dialogTemplate
```

## Template Structure

Each template includes:
- Required #include directives
- Proper namespace usage
- Complete class implementations
- Event handler examples
- Best practice patterns
