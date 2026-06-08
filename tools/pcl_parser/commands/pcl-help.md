# /pcl-help Command

Get help and best practices for PCL development.

## Usage

```
/pcl-help [topic]
```

## Topics

### general (default)
Overall PCL development best practices:
- Code organization
- Memory management
- Error handling
- Performance optimization

### imageProcessing
Image processing guidelines:
- Pixel access patterns
- Color space handling
- Channel selection
- Bulk operations

### threading
Parallel processing best practices:
- Thread creation and management
- Synchronization with Mutex
- Optimal thread count
- Avoiding race conditions

### ui
UI development guidelines:
- Dialog design patterns
- Control usage
- Event handling
- Layout with Sizers

## Examples

```
/pcl-help
/pcl-help imageProcessing
/pcl-help threading
```

## Quick Reference

### Module Architecture
1. MetaModule - Module definition
2. MetaProcess - Process metadata
3. ProcessImplementation - Process logic
4. ProcessInterface - Process GUI

### Essential Headers
```cpp
#include <pcl/Image.h>
#include <pcl/Console.h>
#include <pcl/MetaModule.h>
#include <pcl/MetaProcess.h>
```

### Resources
- PCL Repository: https://gitlab.com/pixinsight/PCL
- Documentation: https://pixinsight.com/developer/pcl/
