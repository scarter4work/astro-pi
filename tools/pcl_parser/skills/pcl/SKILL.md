# PCL Development Skill

You are an expert in developing PixInsight modules using the PCL (PixInsight Class Library).

## PCL Overview

PCL is a C++ framework for building PixInsight modules. It provides:
- High-level abstraction over PixInsight's core application
- Image processing algorithms and data structures
- GUI components for building process interfaces
- Cross-platform compatibility (Linux, macOS, Windows, FreeBSD)

## Key Concepts

### Module Architecture

Every PixInsight module requires:

1. **MetaModule** - Module definition (version, author, etc.)
2. **MetaProcess** - Process metadata (ID, description, categories)
3. **ProcessImplementation** - Actual process logic and parameters
4. **ProcessInterface** - GUI for the process

### Image Classes

- `Image` - Core image class with pixel data
- `ImageWindow` - GUI container for images
- `View` - Reference to image data within a window
- `ImageVariant` - Type-agnostic image reference

### Common Patterns

#### Accessing the Active Image
```cpp
pcl::ImageWindow window = pcl::ImageWindow::ActiveWindow();
pcl::View view = window.MainView();
pcl::Image image;
view.GetImage(image);
// Process image...
view.SetImage(image);
```

#### Console Output
```cpp
pcl::Console console;
console.WriteLn("Processing started...");
console.Note("Information");
console.Warning("Warning message");
console.Critical("Error occurred");
```

#### Progress Monitoring
```cpp
pcl::StandardStatus status;
image.SetStatusCallback(&status);
// Operations will now show progress
```

#### Dialog Creation
```cpp
class MyDialog : public pcl::Dialog {
public:
    MyDialog() {
        SetWindowTitle("My Dialog");
        // Add controls...
        SetSizer(Global_Sizer);
        AdjustToContents();
    }
private:
    pcl::VerticalSizer Global_Sizer;
};
```

## PCL Namespaces

Always use `namespace pcl` or qualify with `pcl::`:

```cpp
using namespace pcl;
// or
pcl::Image image;
pcl::Console console;
```

## Header Files

Include the appropriate headers:

```cpp
#include <pcl/Image.h>          // Image class
#include <pcl/ImageWindow.h>    // ImageWindow class
#include <pcl/Console.h>        // Console output
#include <pcl/Dialog.h>         // Dialog windows
#include <pcl/Sizer.h>          // Layout managers
#include <pcl/Thread.h>         // Threading
#include <pcl/XISF.h>           // XISF file format
#include <pcl/MetaModule.h>     // Module definition
#include <pcl/MetaProcess.h>    // Process definition
```

## Color Spaces

```cpp
ColorSpace::Gray    // Grayscale
ColorSpace::RGB     // RGB color
ColorSpace::CIELab  // CIE L*a*b*
```

## Image Operations

```cpp
image.Apply(value, ImageOp::Add);  // Add value
image.Apply(value, ImageOp::Mul);  // Multiply
image.Normalize();                  // Normalize to [0,1]
image.Rescale();                    // Rescale to [0,1]
image.Invert();                     // Invert values
```

## Threading

For parallel processing:

```cpp
class MyThread : public pcl::Thread {
public:
    void Run() override {
        // Thread work here
    }
};

// Usage
int n = pcl::Thread::NumberOfThreads(dataSize, 16);
// Create and start n threads
```

## Best Practices

1. Always use `StandardStatus` for long operations
2. Implement proper error handling with `pcl::Exception`
3. Use `AutoLock` for thread synchronization
4. Call `ResetSelections()` after channel operations
5. Use `XISFReader`/`XISFWriter` for file I/O
6. Follow the standard module file structure

## Resources

- PCL Repository: https://gitlab.com/pixinsight/PCL
- Documentation: https://pixinsight.com/developer/pcl/
- XISF Specification: https://gitlab.com/pixinsight/XISF-specification
