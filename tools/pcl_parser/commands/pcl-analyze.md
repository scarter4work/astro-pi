# /pcl-analyze Command

Analyze PCL C++ code for structure, includes, classes, and potential issues.

## Usage

```
/pcl-analyze
```

Then paste or provide your PCL C++ code.

## What It Analyzes

- **Includes**: Lists all #include directives and identifies PCL headers
- **Defines**: Extracts #define macros (version info, etc.)
- **Namespaces**: Identifies namespace usage
- **Classes**: Lists class declarations and inheritance
- **Functions**: Extracts function declarations
- **PCL Objects**: Identifies PCL class usage (Image, Console, etc.)
- **Diagnostics**: Reports potential issues and suggestions

## Example Output

```json
{
  "includes": [
    {"file": "pcl/Image.h", "isPCL": true, "line": 1},
    {"file": "pcl/Console.h", "isPCL": true, "line": 2}
  ],
  "classes": [
    {"name": "MyProcess", "baseClasses": ["MetaProcess"], "line": 10}
  ],
  "pclObjects": [
    {"name": "Image", "category": "image"},
    {"name": "Console", "category": "core"}
  ],
  "diagnostics": [
    {"severity": "hint", "message": "Consider using namespace pcl", "line": 1}
  ]
}
```
