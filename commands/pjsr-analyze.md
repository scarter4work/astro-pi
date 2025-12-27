---
description: Analyze PJSR script code for structure, issues, and best practices
---

# Analyze PJSR Code

Analyze the current PJSR script or provided code for:

1. **Script Structure**
   - Feature declarations (#feature-id, #feature-info)
   - Include statements
   - Version checks

2. **Code Quality**
   - beginProcess/endProcess pairing
   - Proper error handling
   - Memory management (jsAutoGC)

3. **API Usage**
   - Identify PJSR classes and objects used
   - Check for deprecated patterns
   - Validate method calls

4. **Best Practices**
   - DPI-scaled dimensions for UI
   - Proper dialog inheritance pattern
   - Console output for user feedback

Use the `pjsr_analyze` tool to get detailed analysis, then provide actionable recommendations.
