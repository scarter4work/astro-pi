# AstroStretch Studio

A React WebView application for PixInsight implementing advanced image stretching algorithms for astrophotography.

![UI Preview](docs/preview.png)

## Algorithms

### Optimal Transport Stretch (OTS)
Based on optimal transport theory (Wasserstein distance minimization), OTS finds the mathematically optimal mapping between your image's histogram and a target distribution optimized for specific astronomical object types.

**Features:**
- Object type presets: Nebula, Galaxy, Star Cluster, Dark Nebula
- Adaptive background targeting (5-30%)
- Blend control between original and full transport
- Highlight protection to prevent clipping
- Chrominance preservation

### Starlet Arctan Stretch (SAS)
Multiscale stretching using the starlet (isotropic undecimated wavelet) transform with arctangent dynamic range compression. Different spatial scales receive different treatments.

**Features:**
- 4-8 wavelet scales using à trous algorithm
- Scale-dependent gain control:
  - Fine scales (1-2): Stars, noise
  - Mid scales (3-4): Fine structure
  - Coarse scales (5-6): Diffuse nebulosity
- Arctangent compression with configurable alpha
- Noise thresholding with MAD estimation
- Background flattening
- Presets: Emission Nebula, Galaxy, Star Cluster, Faint Extended

## UI Features

- Deep space dark theme optimized for astrophotography
- Real-time preview with smooth zoom (scroll wheel) & pan (drag)
- Live histogram visualization comparing source and result
- Hold-to-compare toggle for original/processed view
- Export to PNG

## Development

```bash
# Install dependencies
npm install

# Run development server
npm run dev

# Build for production
npm run build
```

## Deploying to PixInsight

### Option 1: Local WebView (Recommended for Development)

1. Build the project:
   ```bash
   npm run build
   ```

2. The `dist/` folder contains the production build. In your PCL module, load the WebView pointing to the local `index.html`:

   ```cpp
   #include <pcl/WebView.h>

   WebView webView;
   webView.SetContent(File::ReadTextFile("/path/to/astro-stretch-studio/dist/index.html"));
   // Or load from URL:
   webView.LoadContent(String("file:///") + distPath + "/index.html");
   ```

### Option 2: Embedded in PCL Module

1. Build the project
2. Inline the CSS and JS into a single HTML file:

   ```bash
   # Use a bundler or manually inline dist/assets/* into dist/index.html
   ```

3. Embed the HTML as a string resource in your PCL module:

   ```cpp
   static const char* WEBVIEW_HTML = R"(
   <!DOCTYPE html>
   <html>
   <!-- Inlined content here -->
   </html>
   )";

   webView.SetContent(String(WEBVIEW_HTML));
   ```

### Option 3: Hosted WebView

Host the `dist/` contents on a web server and load via URL:

```cpp
webView.LoadContent("https://your-server.com/astro-stretch-studio/");
```

## PCL Bridge Communication

The app includes `pclBridge.ts` for bidirectional communication with PixInsight. On the PCL side, implement these message handlers:

```cpp
// In your PCL module
void OnWebViewMessage(const String& message)
{
    // Parse JSON message
    JSONValue json = JSON::Parse(message);
    String type = json["type"].ToString();

    if (type == "getActiveImageData") {
        // Send image data to WebView
        ImageVariant image = ...;
        // Convert to base64 and send back
    }
    else if (type == "setImageData") {
        // Apply processed image
    }
}

// Send messages to WebView
webView.ExecuteScript("window.postMessage(" + jsonMessage + ", '*')");
```

## Project Structure

```
astro-stretch-studio/
├── src/
│   ├── engines/
│   │   ├── OptimalTransportStretch.ts  # OTS algorithm implementation
│   │   └── StarletArctanStretch.ts     # SAS algorithm implementation
│   ├── components/
│   │   ├── Header.tsx           # Algorithm switcher, export button
│   │   ├── ControlPanel.tsx     # Parameter sliders
│   │   ├── PreviewCanvas.tsx    # Zoomable image preview
│   │   ├── Histogram.tsx        # Histogram visualization
│   │   └── ImageLoader.tsx      # Drag-drop image loader
│   ├── utils/
│   │   └── pclBridge.ts         # PixInsight WebView communication
│   ├── styles/
│   │   └── globals.css          # Dark space theme
│   └── App.tsx                  # Main app with state management
├── dist/                        # Production build
└── package.json
```

## References

### Optimal Transport
- Villani, C. (2008). *Optimal Transport: Old and New*. Springer.
- Peyré, G., & Cuturi, M. (2019). "Computational Optimal Transport." Foundations and Trends in ML.

### Starlet Transform
- Starck, J.-L., Murtagh, F., & Bijaoui, A. (1998). *Image Processing and Data Analysis: The Multiscale Approach*. Cambridge University Press.
- PixInsight ATrousWaveletTransform documentation.

## License

MIT License - See LICENSE file for details.
