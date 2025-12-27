---
description: Generate PJSR code templates for common patterns (script, dialog, image processing, batch)
---

# Generate PJSR Template

Generate a PJSR code template based on what the user needs. Available templates:

1. **scriptTemplate** - Basic script structure with feature declarations, includes, version check, and main function
2. **dialogTemplate** - Dialog with controls, buttons, and sizer layout using __base__ inheritance
3. **imageProcessing** - Image processing with beginProcess/endProcess and proper error handling
4. **batchProcessing** - Batch file processing loop with progress feedback
5. **processExecution** - Execute PixInsight processes programmatically via ProcessInstance
6. **fileIO** - File reading and writing operations

Use the `pjsr_template` tool with the appropriate templateName, then customize the template for the user's specific needs.

Ask the user what kind of script they want to create if not specified.
