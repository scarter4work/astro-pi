# /pcl-class Command

Get detailed documentation for a PCL class.

## Usage

```
/pcl-class <ClassName>
```

## Examples

```
/pcl-class Image
/pcl-class Console
/pcl-class ProcessInterface
```

## Information Provided

- Class description
- Header file location
- Base class (if any)
- Constructors
- Methods with signatures
- Static methods
- Properties

## Common Classes

### Image Processing
- `Image` - Core image class
- `ImageWindow` - Image window container
- `View` - Image view reference
- `ImageVariant` - Type-agnostic image

### Core
- `Console` - Process console output
- `String` - Unicode string
- `Array` - Dynamic array
- `File` - File I/O

### Module Development
- `MetaModule` - Module definition
- `MetaProcess` - Process metadata
- `ProcessImplementation` - Process logic
- `ProcessInterface` - Process GUI

### UI Controls
- `Dialog` - Dialog window
- `Control` - Base widget
- `Sizer` - Layout manager
- `Label`, `Edit`, `PushButton`, etc.
