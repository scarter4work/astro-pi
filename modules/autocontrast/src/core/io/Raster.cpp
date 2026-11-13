// AutoContrast core: raster decoding and load_raster -- io/loaders.py, with the decoders the Python reference
// uses reproduced on libjpeg-turbo, libpng and libtiff (see Raster.h for the presentation rules).
//
// libjpeg and libpng report fatal errors by longjmp. Every call into them happens inside a small function that
// owns the setjmp and holds no object with a destructor, so a longjmp never skips C++ cleanup; the C++ wrappers
// turn the failure into an exception after that function has returned.
#include "core/io/Raster.h"
#include "core/io/Resample.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

#include <jerror.h>
#include <jpeglib.h>
#include <png.h>
#include <tiffio.h>

namespace ac {

namespace {

// ---- paths and the allow-list (Python pathlib semantics) ------------------------------------------------------

const std::set<std::string> kFits = {".fits", ".fit", ".fts"};
const std::set<std::string> kTiff = {".tif", ".tiff"};

// Path(path).name
std::string base_name(std::string p)
{
   while (p.size() > 1 && p.back() == '/')
      p.pop_back();
   const size_t i = p.rfind('/');
   return i == std::string::npos ? p : p.substr(i + 1);
}

// Path(path).suffix (Python 3.14: leading dots are not a suffix; a trailing dot is ".")
std::string py_suffix(const std::string& name)
{
   const size_t start = name.find_first_not_of('.');
   if (start == std::string::npos)
      return "";
   const size_t i = name.rfind('.');
   return (i == std::string::npos || i < start) ? "" : name.substr(i);
}

std::string lower(std::string s)
{
   for (char& ch : s)
      if (ch >= 'A' && ch <= 'Z')
         ch = static_cast<char>(ch - 'A' + 'a');
   return s;
}

// repr() of a Python str (ASCII control characters escaped; other bytes passed through as UTF-8)
std::string py_repr(const std::string& s)
{
   const bool dq = s.find('\'') != std::string::npos && s.find('"') == std::string::npos;
   const char q = dq ? '"' : '\'';
   std::string out(1, q);
   for (unsigned char ch : s) {
      if (ch == '\\') out += "\\\\";
      else if (ch == static_cast<unsigned char>(q)) { out += '\\'; out += q; }
      else if (ch == '\n') out += "\\n";
      else if (ch == '\r') out += "\\r";
      else if (ch == '\t') out += "\\t";
      else if (ch < 0x20 || ch == 0x7f) {
         char buf[8];
         std::snprintf(buf, sizeof buf, "\\x%02x", ch);
         out += buf;
      } else out += static_cast<char>(ch);
   }
   out += q;
   return out;
}

// _require_supported(path)
void require_supported(const std::string& path)
{
   const std::string name = base_name(path);
   const std::string sfx = lower(py_suffix(name));
   const auto supported = supported_suffixes();
   if (supported.count(sfx))
      return;
   std::string list;
   for (const auto& s : supported)
      list += (list.empty() ? "" : ", ") + s;
   throw std::invalid_argument(name + ": unsupported image format " + py_repr(sfx.empty() ? "(no suffix)" : sfx) +
                               "; supported: " + list +
                               ". Other formats are refused because their bit depth is not verified to survive "
                               "decoding, and Pillow would silently reduce them to 8 bits");
}

struct File {
   std::FILE* fp = nullptr;
   File(const std::string& path, const std::string& name)
   {
      fp = std::fopen(path.c_str(), "rb");
      if (fp == nullptr)
         throw std::runtime_error(name + ": cannot open " + path + ": " + std::strerror(errno));
   }
   ~File()
   {
      if (fp != nullptr)
         std::fclose(fp);
   }
   File(const File&) = delete;
   File& operator=(const File&) = delete;
};

std::vector<uint8_t> read_head(const std::string& path, size_t n)
{
   File f(path, base_name(path));
   std::vector<uint8_t> head(n);
   head.resize(std::fread(head.data(), 1, n, f.fp));
   return head;
}

enum class Content { Png, Jpeg, WebP, Tiff, Unknown };

Content sniff(const std::vector<uint8_t>& h)
{
   static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
   if (h.size() >= 8 && std::memcmp(h.data(), png_sig, 8) == 0) return Content::Png;
   if (h.size() >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return Content::Jpeg;
   if (h.size() >= 12 && std::memcmp(h.data(), "RIFF", 4) == 0 && std::memcmp(h.data() + 8, "WEBP", 4) == 0)
      return Content::WebP;
   if (h.size() >= 4 && (std::memcmp(h.data(), "II*\0", 4) == 0 || std::memcmp(h.data(), "MM\0*", 4) == 0))
      return Content::Tiff;
   return Content::Unknown;
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// _png_bit_depth: the IHDR's declared depth, or -1 when the file is not a PNG
int png_bit_depth(const std::string& path)
{
   const auto head = read_head(path, 25);
   static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
   if (head.size() < 25 || std::memcmp(head.data(), sig, 8) != 0 || std::memcmp(head.data() + 12, "IHDR", 4) != 0)
      return -1;
   return head[24];
}

// ---- JPEG (libjpeg-turbo) --------------------------------------------------------------------------------------

struct JpegErr {
   jpeg_error_mgr pub;
   std::jmp_buf jb;
   char msg[JMSG_LENGTH_MAX];
};

void jpeg_error_exit_cb(j_common_ptr cinfo)
{
   auto* e = reinterpret_cast<JpegErr*>(cinfo->err);
   (*cinfo->err->format_message)(cinfo, e->msg);
   std::longjmp(e->jb, 1);
}

// Warnings are ignored as Pillow ignores them (corrupt-data warnings decode on), except a premature end of file:
// Pillow then raises "image file is truncated", and libjpeg would otherwise pad the image with grey.
void jpeg_emit_cb(j_common_ptr cinfo, int level)
{
   if (level < 0 && cinfo->err->msg_code == JWRN_JPEG_EOF) {
      auto* e = reinterpret_cast<JpegErr*>(cinfo->err);
      (*cinfo->err->format_message)(cinfo, e->msg);
      std::longjmp(e->jb, 1);
   }
}

} // namespace

struct JpegReader::Impl {
   jpeg_decompress_struct cinfo;
   JpegErr err;
   std::FILE* fp = nullptr;
   bool created = false;
   int row = 0, width = 0, height = 0, channels = 0;
   bool cmyk = false;
   std::string mode, name;
};

namespace {

// setjmp owners: no locals with destructors
int jpeg_open_c(JpegReader::Impl* d)
{
   if (setjmp(d->err.jb))
      return 1;
   jpeg_create_decompress(&d->cinfo);
   d->created = true;
   jpeg_stdio_src(&d->cinfo, d->fp);
   jpeg_read_header(&d->cinfo, TRUE);
   return 0;
}

int jpeg_start_c(JpegReader::Impl* d, J_COLOR_SPACE out)
{
   if (setjmp(d->err.jb))
      return 1;
   d->cinfo.out_color_space = out;
   jpeg_start_decompress(&d->cinfo);
   return 0;
}

int jpeg_row_c(JpegReader::Impl* d, JSAMPROW row)
{
   if (setjmp(d->err.jb))
      return 1;
   if (jpeg_read_scanlines(&d->cinfo, &row, 1) != 1) {
      std::snprintf(d->err.msg, sizeof d->err.msg, "decoder returned no scanline");
      return 1;
   }
   return 0;
}

} // namespace

JpegReader::JpegReader(const std::string& path) : impl_(new Impl)
{
   std::unique_ptr<Impl> guard(impl_);
   Impl* d = impl_;
   d->name = base_name(path);
   d->fp = std::fopen(path.c_str(), "rb");
   if (d->fp == nullptr)
      throw std::runtime_error(d->name + ": cannot open " + path + ": " + std::strerror(errno));
   d->cinfo.err = jpeg_std_error(&d->err.pub);
   d->err.pub.error_exit = jpeg_error_exit_cb;
   d->err.pub.emit_message = jpeg_emit_cb;
   d->err.msg[0] = '\0';
   auto fail = [&](const std::string& what) {
      if (d->created)
         jpeg_destroy_decompress(&d->cinfo);
      std::fclose(d->fp);
      throw std::runtime_error(d->name + ": " + what);
   };
   if (jpeg_open_c(d) != 0)
      fail(std::string("JPEG decode failed: ") + d->err.msg);
   // JpegImagePlugin SOF handling: 8-bit samples only; 1/3/4 layers -> L/RGB/CMYK, anything else refused
   if (d->cinfo.data_precision != 8)
      fail("cannot handle " + std::to_string(d->cinfo.data_precision) + "-bit layers");
   J_COLOR_SPACE out = JCS_UNKNOWN;
   switch (d->cinfo.num_components) {
   case 1: d->mode = "L"; out = JCS_GRAYSCALE; break;
   case 3: d->mode = "RGB"; out = JCS_RGB; break;
   case 4: d->mode = "CMYK"; out = JCS_CMYK; d->cmyk = true; break;   // rawmode "CMYK;I": Adobe polarity
   default: fail("cannot handle " + std::to_string(d->cinfo.num_components) + "-layer images");
   }
   if (jpeg_start_c(d, out) != 0)
      fail(std::string("JPEG decode failed: ") + d->err.msg);
   d->width = static_cast<int>(d->cinfo.output_width);
   d->height = static_cast<int>(d->cinfo.output_height);
   d->channels = d->cinfo.output_components;
   guard.release();
}

JpegReader::~JpegReader()
{
   if (impl_->created)
      jpeg_destroy_decompress(&impl_->cinfo);
   if (impl_->fp != nullptr)
      std::fclose(impl_->fp);
   delete impl_;
}

namespace {

// The SOF dimensions after jpeg_read_header alone (Pillow's Image.open().size): no decompression is started,
// so a progressive file's coefficients are never buffered.
std::pair<int, int> jpeg_header_dimensions(const std::string& path, const std::string& name)
{
   File f(path, name);
   JpegReader::Impl d;
   d.name = name;
   d.fp = f.fp;
   d.cinfo.err = jpeg_std_error(&d.err.pub);
   d.err.pub.error_exit = jpeg_error_exit_cb;
   d.err.pub.emit_message = jpeg_emit_cb;
   d.err.msg[0] = '\0';
   const int rc = jpeg_open_c(&d);
   const std::pair<int, int> dims{static_cast<int>(d.cinfo.image_width), static_cast<int>(d.cinfo.image_height)};
   const std::string msg = d.err.msg;
   if (d.created)
      jpeg_destroy_decompress(&d.cinfo);
   if (rc != 0)
      throw std::runtime_error(name + ": JPEG decode failed: " + msg);
   return dims;
}

} // namespace

int JpegReader::width() const { return impl_->width; }
int JpegReader::height() const { return impl_->height; }
int JpegReader::channels() const { return impl_->channels; }
const std::string& JpegReader::mode() const { return impl_->mode; }

void JpegReader::read_row(uint8_t* out)
{
   Impl* d = impl_;
   if (d->row >= d->height)
      throw std::logic_error(d->name + ": read past the last JPEG row");
   if (jpeg_row_c(d, out) != 0)
      throw std::runtime_error(d->name + ": JPEG decode failed: " + d->err.msg);
   ++d->row;
   if (d->cmyk) {
      const size_t n = static_cast<size_t>(d->width) * 4;
      for (size_t i = 0; i < n; ++i)
         out[i] = static_cast<uint8_t>(255 - out[i]);
   }
}

Decoded decode_jpeg(const std::string& path)
{
   JpegReader r(path);
   Decoded d;
   d.width = r.width();
   d.height = r.height();
   d.channels = r.channels();
   d.bits = 8;
   d.mode = r.mode();
   const size_t row = static_cast<size_t>(d.width) * d.channels;
   d.u8.resize(row * d.height);
   for (int y = 0; y < d.height; ++y)
      r.read_row(d.u8.data() + row * y);
   return d;
}

// ---- PNG (libpng) ----------------------------------------------------------------------------------------------

namespace {

struct PngCtx {
   png_structp png = nullptr;
   png_infop info = nullptr;
   std::FILE* fp = nullptr;
   char msg[512] = {0};
   png_uint_32 width = 0, height = 0;
   int bit_depth = 0, color_type = 0, interlace = 0;
   size_t rowbytes = 0;
};

void png_error_cb(png_structp png, png_const_charp msg)
{
   auto* c = static_cast<PngCtx*>(png_get_error_ptr(png));
   std::snprintf(c->msg, sizeof c->msg, "%s", msg);
   png_longjmp(png, 1);
}

void png_warning_cb(png_structp, png_const_charp) {}

int png_header_c(PngCtx* c)
{
   if (setjmp(png_jmpbuf(c->png)))
      return 1;
   png_init_io(c->png, c->fp);
   png_read_info(c->png, c->info);
   png_get_IHDR(c->png, c->info, &c->width, &c->height, &c->bit_depth, &c->color_type, &c->interlace, nullptr, nullptr);
   if (c->bit_depth < 8)
      png_set_packing(c->png);   // one sample per byte, unscaled (gray 1/2/4-bit and palette indices)
   if (c->bit_depth == 16)
      png_set_swap(c->png);      // host (little-endian) order
   png_set_interlace_handling(c->png);
   png_read_update_info(c->png, c->info);
   c->rowbytes = png_get_rowbytes(c->png, c->info);
   return 0;
}

int png_rows_c(PngCtx* c, png_bytepp rows)
{
   if (setjmp(png_jmpbuf(c->png)))
      return 1;
   png_read_image(c->png, rows);
   return 0;
}

struct PngChunks {
   bool has_trns = false, has_sbit = false;
   std::vector<uint8_t> trns, sbit;
};

// The ancillary chunks before IDAT, read straight from the file: pypng's tRNS / sBIT handling depends on their
// raw bytes (libpng validates and may drop them).
PngChunks scan_png_chunks(const std::string& path, const std::string& name)
{
   File f(path, name);
   PngChunks out;
   uint8_t sig[8];
   if (std::fread(sig, 1, 8, f.fp) != 8)
      return out;
   for (;;) {
      uint8_t hdr[8];
      if (std::fread(hdr, 1, 8, f.fp) != 8)
         return out;
      const uint32_t len = be32(hdr);
      const std::string type(reinterpret_cast<const char*>(hdr + 4), 4);
      if (type == "IDAT" || type == "IEND")
         return out;
      if (type == "tRNS" || type == "sBIT") {
         std::vector<uint8_t> data(len);
         if (len > 0 && std::fread(data.data(), 1, len, f.fp) != len)
            return out;
         if (type == "tRNS") { out.has_trns = true; out.trns = std::move(data); }
         else { out.has_sbit = true; out.sbit = std::move(data); }
         if (std::fseek(f.fp, 4, SEEK_CUR) != 0)
            return out;
      } else if (std::fseek(f.fp, static_cast<long>(len) + 4, SEEK_CUR) != 0) {
         return out;
      }
   }
}

} // namespace

Decoded decode_png(const std::string& path)
{
   const std::string name = base_name(path);
   File f(path, name);
   PngCtx c;
   c.fp = f.fp;
   c.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &c, png_error_cb, png_warning_cb);
   if (c.png == nullptr)
      throw std::runtime_error(name + ": libpng: cannot create a read struct");
   c.info = png_create_info_struct(c.png);
   struct Destroy {
      PngCtx* c;
      ~Destroy() { png_destroy_read_struct(&c->png, c->info ? &c->info : nullptr, nullptr); }
   } destroy{&c};
   if (c.info == nullptr)
      throw std::runtime_error(name + ": libpng: cannot create an info struct");

   const bool deep = png_bit_depth(path) == 16;
   auto fail = [&](const std::string& why) {
      if (deep)   // _decode_png16: "<name>: could not decode 16-bit PNG: <exc>" (a ValueError)
         throw std::invalid_argument(name + ": could not decode 16-bit PNG: " + why);
      throw std::runtime_error(name + ": PNG decode failed: " + why);
   };
   if (png_header_c(&c) != 0)
      fail(c.msg);

   Decoded d;
   d.width = static_cast<int>(c.width);
   d.height = static_cast<int>(c.height);
   d.bits = c.bit_depth;
   int planes = 0;
   switch (c.color_type) {
   case PNG_COLOR_TYPE_GRAY: planes = 1; break;
   case PNG_COLOR_TYPE_RGB: planes = 3; break;
   case PNG_COLOR_TYPE_PALETTE: planes = 1; break;
   case PNG_COLOR_TYPE_GRAY_ALPHA: planes = 2; break;
   case PNG_COLOR_TYPE_RGB_ALPHA: planes = 4; break;
   default: fail("unknown colour type " + std::to_string(c.color_type));
   }
   const size_t samples = static_cast<size_t>(d.width) * d.height * planes;
   const size_t sample_bytes = c.bit_depth == 16 ? 2 : 1;
   if (c.rowbytes != static_cast<size_t>(d.width) * planes * sample_bytes)
      fail("unexpected row size " + std::to_string(c.rowbytes));

   std::vector<uint8_t> raw(samples * sample_bytes);
   std::vector<png_bytep> rows(static_cast<size_t>(d.height));
   for (int y = 0; y < d.height; ++y)
      rows[static_cast<size_t>(y)] = raw.data() + static_cast<size_t>(y) * c.rowbytes;
   if (png_rows_c(&c, rows.data()) != 0)
      fail(c.msg);

   if (c.bit_depth <= 8) {
      // PngImagePlugin _MODES: (1,0) "1"; (2,0)/(4,0) "L" via L;2 / L;4 (x0x55 / x0x11); (8,0) "L"; (8,2) "RGB";
      // (1..8,3) "P" (indices); (8,4) "LA"; (8,6) "RGBA"
      d.channels = planes;
      d.u8 = std::move(raw);
      switch (c.color_type) {
      case PNG_COLOR_TYPE_GRAY:
         if (c.bit_depth == 1) d.mode = "1";
         else {
            d.mode = "L";
            const unsigned scale = c.bit_depth == 2 ? 0x55u : (c.bit_depth == 4 ? 0x11u : 1u);
            if (scale != 1)
               for (auto& v : d.u8)
                  v = static_cast<uint8_t>(v * scale);
         }
         break;
      case PNG_COLOR_TYPE_RGB: d.mode = "RGB"; break;
      case PNG_COLOR_TYPE_PALETTE: d.mode = "P"; break;
      case PNG_COLOR_TYPE_GRAY_ALPHA: d.mode = "LA"; break;
      default: d.mode = "RGBA"; break;
      }
      return d;
   }

   // 16 bits: pypng Reader.asDirect()
   d.mode = "png16";
   const auto chunks = scan_png_chunks(path, name);
   const uint16_t* s = reinterpret_cast<const uint16_t*>(raw.data());
   const bool trns = chunks.has_trns && (c.color_type == PNG_COLOR_TYPE_GRAY || c.color_type == PNG_COLOR_TYPE_RGB);
   if (chunks.has_trns && !trns)
      fail("tRNS chunk is not valid with colour type " + std::to_string(c.color_type));
   uint16_t key[3] = {0, 0, 0};
   if (trns) {
      if (chunks.trns.size() != static_cast<size_t>(planes) * 2)
         fail("tRNS chunk has the wrong length for colour type " + std::to_string(c.color_type));
      for (int p = 0; p < planes; ++p)
         key[p] = static_cast<uint16_t>((chunks.trns[2 * p] << 8) | chunks.trns[2 * p + 1]);
   }
   d.channels = planes + (trns ? 1 : 0);
   d.u16.resize(static_cast<size_t>(d.width) * d.height * d.channels);
   const size_t npx = static_cast<size_t>(d.width) * d.height;
   for (size_t i = 0; i < npx; ++i) {
      bool opaque = false;
      for (int p = 0; p < planes; ++p) {
         d.u16[i * d.channels + p] = s[i * planes + p];
         opaque = opaque || s[i * planes + p] != key[p];
      }
      if (trns)
         d.u16[i * d.channels + planes] = opaque ? 65535 : 0;
   }
   if (chunks.has_sbit && !chunks.sbit.empty()) {
      const int target = *std::max_element(chunks.sbit.begin(), chunks.sbit.end());
      const int lowest = *std::min_element(chunks.sbit.begin(), chunks.sbit.end());
      if (target > 16)
         fail("sBIT chunk exceeds bitdepth 16");
      if (lowest <= 0)
         fail("sBIT chunk has a 0-entry");
      const int shift = 16 - target;
      if (shift > 0)
         for (auto& v : d.u16)
            v = static_cast<uint16_t>(v >> shift);
   }
   return d;
}

// ---- TIFF (libtiff) --------------------------------------------------------------------------------------------

namespace {

struct TiffMsg {
   std::string first;
};

int tiff_error_cb(TIFF*, void* user, const char* module, const char* fmt, va_list ap)
{
   auto* m = static_cast<TiffMsg*>(user);
   if (m->first.empty()) {
      char buf[512];
      std::vsnprintf(buf, sizeof buf, fmt, ap);
      m->first = (module != nullptr ? std::string(module) + ": " : std::string()) + buf;
   }
   return 1;   // handled: libtiff's global handler (stderr) is not called
}

int tiff_warning_cb(TIFF*, void*, const char*, const char*, va_list) { return 1; }

struct Tiff {
   TIFF* tif = nullptr;
   TiffMsg msg;
   std::string name;

   Tiff(const std::string& path, const std::string& nm) : name(nm)
   {
      { File probe(path, name); }   // a missing file is an I/O error with errno, not a libtiff message
      TIFFOpenOptions* opts = TIFFOpenOptionsAlloc();
      TIFFOpenOptionsSetErrorHandlerExtR(opts, tiff_error_cb, &msg);
      TIFFOpenOptionsSetWarningHandlerExtR(opts, tiff_warning_cb, &msg);
      tif = TIFFOpenExt(path.c_str(), "r", opts);
      TIFFOpenOptionsFree(opts);
      if (tif == nullptr)
         throw std::runtime_error(name + ": TIFF open failed: " + (msg.first.empty() ? "not a TIFF file" : msg.first));
   }
   ~Tiff()
   {
      if (tif != nullptr)
         TIFFClose(tif);
   }
   Tiff(const Tiff&) = delete;
   Tiff& operator=(const Tiff&) = delete;

   [[noreturn]] void fail(const std::string& what) const
   {
      throw std::runtime_error(name + ": TIFF decode failed: " + what + (msg.first.empty() ? "" : " (" + msg.first + ")"));
   }
};

struct PageInfo {
   uint32_t width = 0, height = 0;
   uint16_t spp = 1, bps = 1, format = SAMPLEFORMAT_UINT, planar = PLANARCONFIG_CONTIG;
   uint32_t subfile = 0;
   bool same_shape(const PageInfo& o) const
   {
      return width == o.width && height == o.height && spp == o.spp && bps == o.bps && format == o.format;
   }
};

PageInfo page_info(TIFF* tif)
{
   PageInfo p;
   TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &p.width);
   TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &p.height);
   TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &p.spp);
   TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &p.bps);
   TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &p.format);
   TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &p.planar);
   TIFFGetFieldDefaulted(tif, TIFFTAG_SUBFILETYPE, &p.subfile);
   return p;
}

std::string py_tuple(const std::vector<long>& v)
{
   std::string s = "(";
   for (size_t i = 0; i < v.size(); ++i)
      s += (i ? ", " : "") + std::to_string(v[i]);
   return s + (v.size() == 1 ? ",)" : ")");
}

} // namespace

Decoded decode_tiff(const std::string& path)
{
   const std::string name = base_name(path);
   Tiff t(path, name);
   const PageInfo p0 = page_info(t.tif);

   // tifffile.imread returns the first series: full-resolution pages of the same shape and dtype are stacked
   // into one array, which the Python loader refuses beyond three dimensions. Refuse any such stack (also the
   // gray one Python would misread as rows); reduced-resolution pages (thumbnails) are not part of it.
   long stacked = 1;
   const tdir_t ndirs = TIFFNumberOfDirectories(t.tif);
   for (tdir_t dir = 1; dir < ndirs; ++dir) {
      if (!TIFFSetDirectory(t.tif, dir))
         t.fail("cannot read page " + std::to_string(dir));
      const PageInfo pi = page_info(t.tif);
      if ((pi.subfile & FILETYPE_REDUCEDIMAGE) == 0 && pi.same_shape(p0))
         ++stacked;
   }
   if (stacked > 1) {
      std::vector<long> shape = {stacked, static_cast<long>(p0.height), static_cast<long>(p0.width)};
      if (p0.spp > 1)
         shape.push_back(p0.spp);
      throw std::invalid_argument(name + ": expected a single 2-D or 3-D TIFF page, got shape " + py_tuple(shape) +
                                  "; the sidecar has no rule for choosing among planes");
   }
   if (!TIFFSetDirectory(t.tif, 0))
      t.fail("cannot return to page 0");
   if (p0.planar == PLANARCONFIG_SEPARATE && p0.spp > 1)
      throw std::invalid_argument(name + ": planar (separate-plane) TIFF is not supported: tifffile returns it "
                                         "channel-first, shape " +
                                  py_tuple({p0.spp, static_cast<long>(p0.height), static_cast<long>(p0.width)}) +
                                  ", which the Python loader would read as rows");
   int bytes = 0;
   if (p0.bps == 8 && p0.format == SAMPLEFORMAT_UINT) bytes = 1;
   else if (p0.bps == 16 && p0.format == SAMPLEFORMAT_UINT) bytes = 2;
   else if (p0.bps == 32 && p0.format == SAMPLEFORMAT_IEEEFP) bytes = 4;
   else
      throw std::invalid_argument(name + ": unsupported TIFF samples (" + std::to_string(p0.bps) + "-bit, sample format " +
                                  std::to_string(p0.format) + "); the loader reads 8/16-bit unsigned integer and "
                                                              "32-bit float TIFF");
   if (p0.width == 0 || p0.height == 0 || p0.spp == 0)
      t.fail("empty image");

   Decoded d;
   d.width = static_cast<int>(p0.width);
   d.height = static_cast<int>(p0.height);
   d.channels = p0.spp;
   d.bits = p0.bps;
   d.mode = "tiff";
   const size_t px_bytes = static_cast<size_t>(p0.spp) * bytes;
   const size_t row_bytes = static_cast<size_t>(p0.width) * px_bytes;
   std::vector<uint8_t> buf(row_bytes * p0.height);

   if (TIFFIsTiled(t.tif)) {
      uint32_t tw = 0, th = 0;
      TIFFGetField(t.tif, TIFFTAG_TILEWIDTH, &tw);
      TIFFGetField(t.tif, TIFFTAG_TILELENGTH, &th);
      if (tw == 0 || th == 0)
         t.fail("tile size 0");
      const tmsize_t tile_bytes = TIFFTileSize(t.tif);
      if (tile_bytes != static_cast<tmsize_t>(static_cast<size_t>(tw) * th * px_bytes))
         t.fail("unexpected tile size");
      std::vector<uint8_t> tile(static_cast<size_t>(tile_bytes));
      for (uint32_t y0 = 0; y0 < p0.height; y0 += th)
         for (uint32_t x0 = 0; x0 < p0.width; x0 += tw) {
            const ttile_t idx = TIFFComputeTile(t.tif, x0, y0, 0, 0);
            if (TIFFReadEncodedTile(t.tif, idx, tile.data(), tile_bytes) < 0)
               t.fail("tile " + std::to_string(idx));
            const uint32_t rows = std::min(th, p0.height - y0), cols = std::min(tw, p0.width - x0);
            for (uint32_t r = 0; r < rows; ++r)
               std::memcpy(buf.data() + (y0 + r) * row_bytes + x0 * px_bytes, tile.data() + r * tw * px_bytes,
                           cols * px_bytes);
         }
   } else {
      uint32_t rps = 0;
      TIFFGetFieldDefaulted(t.tif, TIFFTAG_ROWSPERSTRIP, &rps);
      if (rps == 0 || rps > p0.height)
         rps = p0.height;
      const tstrip_t strips = TIFFNumberOfStrips(t.tif);
      for (tstrip_t s = 0; s < strips; ++s) {
         const uint32_t y0 = s * rps;
         if (y0 >= p0.height)
            break;
         const uint32_t rows = std::min(rps, p0.height - y0);
         const tmsize_t want = static_cast<tmsize_t>(rows * row_bytes);
         const tmsize_t got = TIFFReadEncodedStrip(t.tif, s, buf.data() + y0 * row_bytes, want);
         if (got != want)
            t.fail("strip " + std::to_string(s) + (got < 0 ? "" : " short (" + std::to_string(got) + " bytes)"));
      }
   }
   if (bytes == 1) {
      d.u8 = std::move(buf);
   } else if (bytes == 2) {
      d.u16.resize(buf.size() / 2);
      std::memcpy(d.u16.data(), buf.data(), buf.size());
   } else {
      d.f32.resize(buf.size() / 4);
      std::memcpy(d.f32.data(), buf.data(), buf.size());
   }
   return d;
}

// ---- dimensions -----------------------------------------------------------------------------------------------

namespace {

std::pair<int, int> fits_dimensions(const std::string& path, const std::string& name)
{
   File f(path, name);
   long naxis1 = -1, naxis2 = -1;
   char card[81];
   card[80] = '\0';
   for (int n = 0; n < 2880 * 64 / 80; ++n) {
      if (std::fread(card, 1, 80, f.fp) != 80)
         break;
      const std::string key = std::string(card, 8);
      if (key.rfind("END", 0) == 0 && key.find_first_not_of(' ', 3) == std::string::npos)
         break;
      if (card[8] != '=' || card[9] != ' ')
         continue;
      if (key == "NAXIS1  ") naxis1 = std::strtol(card + 10, nullptr, 10);
      else if (key == "NAXIS2  ") naxis2 = std::strtol(card + 10, nullptr, 10);
   }
   if (naxis1 < 0 || naxis2 < 0)
      throw std::invalid_argument(name + ": FITS primary header has no NAXIS1/NAXIS2");
   return {static_cast<int>(naxis1), static_cast<int>(naxis2)};
}

std::pair<int, int> webp_dimensions(const std::vector<uint8_t>& h, const std::string& name)
{
   auto le16 = [&](size_t i) { return static_cast<int>(h[i] | (h[i + 1] << 8)); };
   auto le24 = [&](size_t i) { return static_cast<int>(h[i] | (h[i + 1] << 8) | (h[i + 2] << 16)); };
   if (h.size() >= 30 && std::memcmp(h.data() + 12, "VP8 ", 4) == 0 && h[23] == 0x9d && h[24] == 0x01 && h[25] == 0x2a)
      return {le16(26) & 0x3fff, le16(28) & 0x3fff};
   if (h.size() >= 25 && std::memcmp(h.data() + 12, "VP8L", 4) == 0 && h[20] == 0x2f) {
      const uint32_t b = h[21] | (h[22] << 8) | (h[23] << 16) | (uint32_t(h[24]) << 24);
      return {static_cast<int>(b & 0x3fff) + 1, static_cast<int>((b >> 14) & 0x3fff) + 1};
   }
   if (h.size() >= 30 && std::memcmp(h.data() + 12, "VP8X", 4) == 0)
      return {le24(24) + 1, le24(27) + 1};
   throw std::invalid_argument(name + ": unrecognised WebP header");
}

} // namespace

std::set<std::string> supported_suffixes()
{
   return {".fits", ".fit", ".fts", ".tif", ".tiff", ".png", ".jpg", ".jpeg", ".jpe", ".jfif", ".webp"};
}

std::pair<int, int> image_dimensions(const std::string& path)
{
   require_supported(path);
   const std::string name = base_name(path);
   const std::string sfx = lower(py_suffix(name));
   if (kFits.count(sfx))
      return fits_dimensions(path, name);
   const auto head = read_head(path, 32);
   const Content kind = kTiff.count(sfx) ? Content::Tiff : sniff(head);
   switch (kind) {
   case Content::Tiff: {
      Tiff t(path, name);
      const PageInfo p = page_info(t.tif);
      return {static_cast<int>(p.width), static_cast<int>(p.height)};
   }
   case Content::Png:
      if (head.size() < 24 || std::memcmp(head.data() + 12, "IHDR", 4) != 0)
         throw std::invalid_argument(name + ": PNG without an IHDR chunk");
      return {static_cast<int>(be32(head.data() + 16)), static_cast<int>(be32(head.data() + 20))};
   case Content::Jpeg:
      return jpeg_header_dimensions(path, name);
   case Content::WebP:
      return webp_dimensions(head, name);
   default:
      throw std::invalid_argument("cannot identify image file " + py_repr(path));
   }
}

double downsample_factor(const std::string& path, const Image& loaded)
{
   const auto [w, h] = image_dimensions(path);
   return static_cast<double>(std::max(w, h)) / std::max(loaded.width, loaded.height);
}

// ---- load_raster ----------------------------------------------------------------------------------------------

namespace {

// (max(1, round(w * s)), max(1, round(h * s))) with s = max_dim / max(w, h): Python round() is half-to-even
std::pair<int, int> target_size(int w, int h, int max_dim)
{
   const double scale = static_cast<double>(max_dim) / std::max(w, h);
   const int nw = std::max(1, static_cast<int>(std::nearbyint(w * scale)));
   const int nh = std::max(1, static_cast<int>(std::nearbyint(h * scale)));
   return {nw, nh};
}

bool needs_resize(int w, int h, std::optional<int> max_dim) { return max_dim && std::max(w, h) > *max_dim; }

// An (H, W, C) array widened to double, before load_raster's promotion / alpha / normalisation.
struct Array {
   int width = 0, height = 0, channels = 0;
   std::vector<double> v;
};

// _resize_native: each plane resized as a float32 "F" image when over max_dim, else the native values as is.
template <class T>
Array native_planes(const T* src, int w, int h, int c, const std::vector<int>& keep, std::optional<int> max_dim)
{
   Array a;
   const int k = static_cast<int>(keep.size());
   const size_t npx = static_cast<size_t>(w) * h;
   if (!needs_resize(w, h, max_dim)) {
      a.width = w;
      a.height = h;
      a.channels = k;
      a.v.resize(npx * k);
      for (size_t i = 0; i < npx; ++i)
         for (int j = 0; j < k; ++j)
            a.v[i * k + j] = static_cast<double>(src[i * c + keep[static_cast<size_t>(j)]]);
      return a;
   }
   const auto [nw, nh] = target_size(w, h, *max_dim);
   a.width = nw;
   a.height = nh;
   a.channels = k;
   a.v.resize(static_cast<size_t>(nw) * nh * k);
   std::vector<float> plane(npx);
   for (int j = 0; j < k; ++j) {
      for (size_t i = 0; i < npx; ++i)
         plane[i] = static_cast<float>(src[i * c + keep[static_cast<size_t>(j)]]);   // .astype(np.float32)
      const auto r = resize_f32(plane.data(), w, h, nw, nh);
      for (size_t i = 0; i < r.size(); ++i)
         a.v[i * k + j] = static_cast<double>(r[i]);
   }
   return a;
}

Array from_u8(std::vector<uint8_t>&& px, int w, int h, int c)
{
   Array a;
   a.width = w;
   a.height = h;
   a.channels = c;
   a.v.resize(px.size());
   for (size_t i = 0; i < px.size(); ++i)
      a.v[i] = px[i];
   return a;
}

// Pillow's RGBA -> RGBa / LA -> La (premultiply) and back, around the resampler (Image.resize).
void premultiply(std::vector<uint8_t>& px, int c)
{
   for (size_t i = 0; i < px.size(); i += c) {
      const unsigned alpha = px[i + c - 1];
      for (int ch = 0; ch < c - 1; ++ch) {
         const unsigned tmp = px[i + ch] * alpha + 128;   // MULDIV255
         px[i + ch] = static_cast<uint8_t>(((tmp >> 8) + tmp) >> 8);
      }
   }
}

void unpremultiply(std::vector<uint8_t>& px, int c)
{
   for (size_t i = 0; i < px.size(); i += c) {
      const unsigned alpha = px[i + c - 1];
      if (alpha == 255 || alpha == 0)
         continue;
      for (int ch = 0; ch < c - 1; ++ch) {
         const unsigned v = (255 * static_cast<unsigned>(px[i + ch])) / alpha;   // CLIP8
         px[i + ch] = static_cast<uint8_t>(v < 256 ? v : 255);
      }
   }
}

// The 8-bit Pillow path: Image.open -> [im.resize(new_size)] -> np.asarray. Sets full_scale (255) except for
// mode "1", which numpy holds as bool (no full scale; the 0/1 values pass the range inference untouched).
Array pillow_path(const std::string& path, const std::string& name, const std::string& sfx, std::optional<int> max_dim,
                  std::optional<double>& full_scale)
{
   const Content kind = sniff(read_head(path, 16));
   if (kind == Content::Jpeg) {
      JpegReader r(path);
      const int w = r.width(), h = r.height(), c = r.channels();
      full_scale = 255.0;
      std::vector<uint8_t> row(static_cast<size_t>(w) * c);
      if (needs_resize(w, h, max_dim)) {
         const auto [nw, nh] = target_size(w, h, *max_dim);
         ResizeU8Stream s(w, h, c, nw, nh);
         for (int y = 0; y < h; ++y) {
            r.read_row(row.data());
            s.push_row(row.data());
         }
         return from_u8(s.finish(), nw, nh, c);
      }
      std::vector<uint8_t> px(static_cast<size_t>(w) * h * c);
      for (int y = 0; y < h; ++y)
         r.read_row(px.data() + static_cast<size_t>(y) * w * c);
      return from_u8(std::move(px), w, h, c);
   }
   if (kind == Content::Png) {
      Decoded d = decode_png(path);
      if (d.u8.empty())   // a 16-bit PNG whose IHDR is not where _png_bit_depth looks: libpng still decodes it
         throw std::invalid_argument(name + ": PNG IHDR is not the first chunk; refusing an 8-bit read of " +
                                     std::to_string(d.bits) + "-bit data");
      full_scale = d.mode == "1" ? std::nullopt : std::optional<double>(255.0);
      if (!needs_resize(d.width, d.height, max_dim))
         return from_u8(std::move(d.u8), d.width, d.height, d.channels);
      const auto [nw, nh] = target_size(d.width, d.height, *max_dim);
      if (d.mode == "1" || d.mode == "P")
         return from_u8(resize_nearest_u8(d.u8.data(), d.width, d.height, d.channels, nw, nh), nw, nh, d.channels);
      if (d.mode == "LA" || d.mode == "RGBA") {
         premultiply(d.u8, d.channels);
         auto out = resize_u8(d.u8.data(), d.width, d.height, d.channels, nw, nh);
         unpremultiply(out, d.channels);
         return from_u8(std::move(out), nw, nh, d.channels);
      }
      return from_u8(resize_u8(d.u8.data(), d.width, d.height, d.channels, nw, nh), nw, nh, d.channels);
   }
   if (kind == Content::WebP)
      throw std::invalid_argument(name + ": WebP pixels cannot be read by the native AutoContrast loader (no WebP "
                                         "decoder is built in); convert the render to PNG or TIFF");
   if (kind == Content::Tiff)
      throw std::invalid_argument(name + ": TIFF data under a " + py_repr(sfx) +
                                  " suffix; rename it .tif so it is read at native depth");
   throw std::invalid_argument("cannot identify image file " + py_repr(path));
}

} // namespace

Image load_raster(const std::string& path, std::optional<int> max_dim)
{
   require_supported(path);
   const std::string name = base_name(path);
   const std::string sfx = lower(py_suffix(name));
   if (kFits.count(sfx))
      throw std::invalid_argument(name + ": FITS pixels open through PixInsight (load_image); load_raster reads "
                                         "TIFF, PNG and JPEG");
   if (max_dim && *max_dim < 1)
      throw std::invalid_argument(name + ": max_dim must be positive, got " + std::to_string(*max_dim));

   Array a;
   std::optional<double> full_scale;
   if (kTiff.count(sfx)) {
      const Decoded d = decode_tiff(path);
      // every plane but a 4th (dropped below as alpha) is resized; channel order kept
      std::vector<int> keep;
      for (int ch = 0; ch < d.channels; ++ch)
         if (!(d.channels == 4 && ch == 3))
            keep.push_back(ch);
      if (d.bits == 8) { a = native_planes(d.u8.data(), d.width, d.height, d.channels, keep, max_dim); full_scale = 255.0; }
      else if (d.bits == 16) { a = native_planes(d.u16.data(), d.width, d.height, d.channels, keep, max_dim); full_scale = 65535.0; }
      else a = native_planes(d.f32.data(), d.width, d.height, d.channels, keep, max_dim);
   } else if (png_bit_depth(path) == 16) {
      const Decoded d = decode_png(path);
      // gray(+alpha) -> one plane; RGB(+alpha) -> three
      const std::vector<int> keep = d.channels <= 2 ? std::vector<int>{0} : std::vector<int>{0, 1, 2};
      a = native_planes(d.u16.data(), d.width, d.height, d.channels, keep, max_dim);
      full_scale = 65535.0;
   } else {
      a = pillow_path(path, name, sfx, max_dim, full_scale);
      if (a.channels == 4) {   // RGBA / CMYK: drop the fourth channel
         Array b;
         b.width = a.width;
         b.height = a.height;
         b.channels = 3;
         b.v.resize(static_cast<size_t>(a.width) * a.height * 3);
         for (size_t i = 0, n = static_cast<size_t>(a.width) * a.height; i < n; ++i)
            for (int ch = 0; ch < 3; ++ch)
               b.v[i * 3 + ch] = a.v[i * 4 + ch];
         a = std::move(b);
      }
   }

   // ndim == 2 -> stack([arr] * 3)
   const int out_c = a.channels == 1 ? 3 : a.channels;
   Image img(a.width, a.height, out_c);
   const size_t npx = static_cast<size_t>(a.width) * a.height;
   for (size_t i = 0; i < npx; ++i)
      for (int ch = 0; ch < out_c; ++ch)
         img.data[i * out_c + ch] = a.v[i * a.channels + (a.channels == 1 ? 0 : ch)];
   a.v.clear();
   a.v.shrink_to_fit();

   if (full_scale) {
      for (double& v : img.data)
         v = v / *full_scale;
   } else {
      // no known full scale (float data, mode "1"): infer the smallest standard range that fits; numpy's max
      // propagates NaN, and NaN > 1.0 is false
      double max_value = img.data.empty() ? 1.0 : -std::numeric_limits<double>::infinity();
      for (double v : img.data) {
         if (std::isnan(v)) {
            max_value = v;
            break;
         }
         max_value = std::max(max_value, v);
      }
      if (max_value > 1.0) {
         const double divisor = max_value > 255.0 ? 65535.0 : 255.0;
         for (double& v : img.data)
            v = v / divisor;
      }
   }
   // np.clip(arr, 0, 1): x < 0 -> 0, x > 1 -> 1, anything else (NaN, -0.0) unchanged -- the bytes numpy writes
   for (double& v : img.data)
      v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
   return img;
}

} // namespace ac
