#include "mmdvr/SphericalMp4Inject.hpp"
#include "mmdvr/Diagnostics.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace mmdvr {
namespace {

// Google Spherical Video UUID (spatial-media).
static const uint8_t kSphericalUuid[16] = {
    0xff, 0xcc, 0x82, 0x63, 0xf8, 0x55, 0x4a, 0x93,
    0x88, 0x14, 0x58, 0x7a, 0x02, 0x52, 0x1f, 0xdd};

static uint32_t rdBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint64_t rdBe64(const uint8_t* p) {
  return (uint64_t(rdBe32(p)) << 32) | uint64_t(rdBe32(p + 4));
}
static void wrBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t((v >> 24) & 0xff);
  p[1] = uint8_t((v >> 16) & 0xff);
  p[2] = uint8_t((v >> 8) & 0xff);
  p[3] = uint8_t(v & 0xff);
}
static void wrBe64(uint8_t* p, uint64_t v) {
  wrBe32(p, uint32_t(v >> 32));
  wrBe32(p + 4, uint32_t(v & 0xffffffffu));
}

static bool fourccEq(const uint8_t* p, const char* tag) {
  return p[0] == (uint8_t)tag[0] && p[1] == (uint8_t)tag[1] &&
         p[2] == (uint8_t)tag[2] && p[3] == (uint8_t)tag[3];
}

struct BoxLoc {
  size_t start = 0;   // offset of size field
  size_t header = 8;  // 8 or 16
  size_t size = 0;    // total box size including header
  char type[5]{};
};

static bool readBox(const std::vector<uint8_t>& data, size_t pos, size_t end, BoxLoc& out) {
  if (pos + 8 > end) return false;
  uint32_t size32 = rdBe32(data.data() + pos);
  out.start = pos;
  out.header = 8;
  std::memcpy(out.type, data.data() + pos + 4, 4);
  out.type[4] = 0;
  if (size32 == 1) {
    if (pos + 16 > end) return false;
    out.header = 16;
    out.size = (size_t)rdBe64(data.data() + pos + 8);
  } else if (size32 == 0) {
    out.size = end - pos;
  } else {
    out.size = size32;
  }
  if (out.size < out.header || pos + out.size > end) return false;
  return true;
}

static std::string makeSphericalXml(VrMode vrMode, StereoMode stereo, int frameW, int frameH) {
  std::ostringstream xml;
  xml << "<?xml version=\"1.0\"?>"
         "<rdf:SphericalVideo "
         "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\" "
         "xmlns:GSpherical=\"http://ns.google.com/videos/1.0/spherical/\">"
         "<GSpherical:Spherical>true</GSpherical:Spherical>"
         "<GSpherical:Stitched>true</GSpherical:Stitched>"
         "<GSpherical:StitchingSoftware>MMD-VR-Renderer</GSpherical:StitchingSoftware>"
         "<GSpherical:ProjectionType>equirectangular</GSpherical:ProjectionType>";
  if (stereo == StereoMode::Stereo)
    xml << "<GSpherical:StereoMode>left-right</GSpherical:StereoMode>";
  // VR180: describe this frame as the centre half of a full 360×180 equirect.
  if (vrMode == VrMode::VR180 && frameW > 0 && frameH > 0) {
    const int cropW = frameW;
    const int cropH = frameH;
    const int fullW = frameW * 2;
    const int fullH = frameH;
    const int left = (fullW - cropW) / 2;
    const int top = 0;
    xml << "<GSpherical:CroppedAreaImageWidthPixels>" << cropW << "</GSpherical:CroppedAreaImageWidthPixels>"
           "<GSpherical:CroppedAreaImageHeightPixels>" << cropH << "</GSpherical:CroppedAreaImageHeightPixels>"
           "<GSpherical:FullPanoWidthPixels>" << fullW << "</GSpherical:FullPanoWidthPixels>"
           "<GSpherical:FullPanoHeightPixels>" << fullH << "</GSpherical:FullPanoHeightPixels>"
           "<GSpherical:CroppedAreaLeftPixels>" << left << "</GSpherical:CroppedAreaLeftPixels>"
           "<GSpherical:CroppedAreaTopPixels>" << top << "</GSpherical:CroppedAreaTopPixels>";
  }
  xml << "</rdf:SphericalVideo>";
  return xml.str();
}

static std::vector<uint8_t> makeUuidBox(const std::string& xml) {
  const size_t content = 16 + xml.size();
  const size_t total = 8 + content;
  std::vector<uint8_t> box(total);
  wrBe32(box.data(), (uint32_t)total);
  box[4] = 'u'; box[5] = 'u'; box[6] = 'i'; box[7] = 'd';
  std::memcpy(box.data() + 8, kSphericalUuid, 16);
  std::memcpy(box.data() + 24, xml.data(), xml.size());
  return box;
}

static bool isVideoTrak(const std::vector<uint8_t>& data, const BoxLoc& trak) {
  size_t pos = trak.start + trak.header;
  const size_t end = trak.start + trak.size;
  while (pos + 8 <= end) {
    BoxLoc b;
    if (!readBox(data, pos, end, b)) break;
    if (fourccEq((const uint8_t*)b.type, "mdia")) {
      size_t mpos = b.start + b.header;
      const size_t mend = b.start + b.size;
      while (mpos + 8 <= mend) {
        BoxLoc m;
        if (!readBox(data, mpos, mend, m)) break;
        if (fourccEq((const uint8_t*)m.type, "hdlr")) {
          // hdlr: ver(1)+flags(3)+pre(4)+handler(4) ...
          if (m.header + 12 <= m.size) {
            const uint8_t* handler = data.data() + m.start + m.header + 8;
            if (fourccEq(handler, "vide")) return true;
          }
        }
        mpos = m.start + m.size;
      }
    }
    pos = b.start + b.size;
  }
  return false;
}

static void patchChunkOffsets(std::vector<uint8_t>& data, size_t moovStart, size_t moovEnd, int64_t delta) {
  if (delta == 0) return;
  std::vector<size_t> stack = {moovStart};
  while (!stack.empty()) {
    size_t pos = stack.back();
    stack.pop_back();
    BoxLoc b;
    if (!readBox(data, pos, moovEnd, b)) continue;
    const size_t payload = b.start + b.header;
    const size_t bend = b.start + b.size;
    if (fourccEq((const uint8_t*)b.type, "stco")) {
      // ver/flags(4) + entry_count(4) + entries(4*n)
      if (payload + 8 <= bend) {
        uint32_t n = rdBe32(data.data() + payload + 4);
        uint8_t* ent = data.data() + payload + 8;
        for (uint32_t i = 0; i < n && ent + 4 <= data.data() + bend; ++i, ent += 4) {
          uint32_t v = rdBe32(ent);
          wrBe32(ent, uint32_t(int64_t(v) + delta));
        }
      }
    } else if (fourccEq((const uint8_t*)b.type, "co64")) {
      if (payload + 8 <= bend) {
        uint32_t n = rdBe32(data.data() + payload + 4);
        uint8_t* ent = data.data() + payload + 8;
        for (uint32_t i = 0; i < n && ent + 8 <= data.data() + bend; ++i, ent += 8) {
          uint64_t v = rdBe64(ent);
          wrBe64(ent, uint64_t(int64_t(v) + delta));
        }
      }
    } else if (fourccEq((const uint8_t*)b.type, "moov") || fourccEq((const uint8_t*)b.type, "trak") ||
               fourccEq((const uint8_t*)b.type, "mdia") || fourccEq((const uint8_t*)b.type, "minf") ||
               fourccEq((const uint8_t*)b.type, "stbl") || fourccEq((const uint8_t*)b.type, "edts") ||
               fourccEq((const uint8_t*)b.type, "udta") || fourccEq((const uint8_t*)b.type, "meta")) {
      size_t c = payload;
      while (c + 8 <= bend) {
        BoxLoc ch;
        if (!readBox(data, c, bend, ch)) break;
        stack.push_back(ch.start);
        c = ch.start + ch.size;
      }
    }
    // continue scanning siblings only from root calls — recursive via stack of children above
  }
}

// Grow size fields of ancestors from leafStart up through containers that contain it.
static void growAncestors(std::vector<uint8_t>& data, size_t fileEnd, size_t leafStart, size_t leafEnd, uint32_t add) {
  size_t pos = 0;
  while (pos + 8 <= fileEnd) {
    BoxLoc b;
    if (!readBox(data, pos, fileEnd, b)) break;
    if (leafStart >= b.start && leafEnd <= b.start + b.size) {
      // This box contains the leaf — grow it (except the leaf itself handled separately).
      if (!(leafStart == b.start && leafEnd == b.start + b.size)) {
        if (b.header == 8) {
          uint32_t sz = rdBe32(data.data() + b.start);
          if (sz != 0 && sz != 1) wrBe32(data.data() + b.start, sz + add);
        } else if (b.header == 16) {
          uint64_t sz = rdBe64(data.data() + b.start + 8);
          wrBe64(data.data() + b.start + 8, sz + add);
        }
        // Recurse into children to grow intermediate containers.
        size_t c = b.start + b.header;
        const size_t bend = b.start + b.size;
        while (c + 8 <= bend) {
          BoxLoc ch;
          if (!readBox(data, c, bend, ch)) break;
          if (leafStart >= ch.start && leafEnd <= ch.start + ch.size &&
              !(leafStart == ch.start && leafEnd == ch.start + ch.size)) {
            growAncestors(data, fileEnd, leafStart, leafEnd, add);
            // Also grow this child
            if (ch.header == 8) {
              uint32_t sz = rdBe32(data.data() + ch.start);
              if (sz != 0 && sz != 1) wrBe32(data.data() + ch.start, sz + add);
            } else if (ch.header == 16) {
              uint64_t sz = rdBe64(data.data() + ch.start + 8);
              wrBe64(data.data() + ch.start + 8, sz + add);
            }
          }
          c = ch.start + ch.size;
        }
      }
      return;
    }
    pos = b.start + b.size;
  }
}

// Simpler grow: walk path moov->trak and add to each size field on the path.
static bool growPathSizes(std::vector<uint8_t>& data, const std::vector<size_t>& pathStarts, uint32_t add) {
  for (size_t start : pathStarts) {
    BoxLoc b;
    if (!readBox(data, start, data.size(), b)) return false;
    if (b.header == 8) {
      uint32_t sz = rdBe32(data.data() + b.start);
      if (sz == 0 || sz == 1) return false;
      wrBe32(data.data() + b.start, sz + add);
    } else {
      uint64_t sz = rdBe64(data.data() + b.start + 8);
      wrBe64(data.data() + b.start + 8, sz + add);
    }
  }
  return true;
}

static bool findVideoTrakPath(const std::vector<uint8_t>& data, size_t moovStart, size_t moovEnd,
                             std::vector<size_t>& pathOut, size_t& trakEndOut) {
  BoxLoc moov;
  if (!readBox(data, moovStart, moovEnd, moov)) return false;
  pathOut = {moov.start};
  size_t pos = moov.start + moov.header;
  while (pos + 8 <= moov.start + moov.size) {
    BoxLoc trak;
    if (!readBox(data, pos, moov.start + moov.size, trak)) break;
    if (fourccEq((const uint8_t*)trak.type, "trak") && isVideoTrak(data, trak)) {
      pathOut.push_back(trak.start);
      trakEndOut = trak.start + trak.size;
      return true;
    }
    pos = trak.start + trak.size;
  }
  return false;
}

} // namespace

bool injectSphericalMp4(
    const std::wstring& inMp4,
    const std::wstring& outMp4,
    VrMode vrMode,
    StereoMode stereoMode,
    int frameW,
    int frameH,
    std::wstring* errorOut) {
  std::ifstream in(inMp4, std::ios::binary);
  if (!in) {
    if (errorOut) *errorOut = L"cannot open input mp4";
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  if (data.size() < 16) {
    if (errorOut) *errorOut = L"mp4 too small";
    return false;
  }

  // Locate moov / mdat
  size_t moovStart = 0, moovSize = 0, mdatStart = 0;
  bool haveMoov = false, haveMdat = false;
  size_t pos = 0;
  while (pos + 8 <= data.size()) {
    BoxLoc b;
    if (!readBox(data, pos, data.size(), b)) break;
    if (fourccEq((const uint8_t*)b.type, "moov")) {
      moovStart = b.start;
      moovSize = b.size;
      haveMoov = true;
    } else if (fourccEq((const uint8_t*)b.type, "mdat") && !haveMdat) {
      mdatStart = b.start;
      haveMdat = true;
    }
    pos = b.start + b.size;
  }
  if (!haveMoov) {
    if (errorOut) *errorOut = L"moov not found";
    return false;
  }

  // Remove existing spherical uuid in video trak (optional skip — append another is ok but messy)
  std::vector<size_t> path;
  size_t trakEnd = 0;
  if (!findVideoTrakPath(data, moovStart, moovStart + moovSize, path, trakEnd)) {
    if (errorOut) *errorOut = L"video trak not found";
    return false;
  }

  auto uuidBox = makeUuidBox(makeSphericalXml(vrMode, stereoMode, frameW, frameH));
  const uint32_t add = (uint32_t)uuidBox.size();

  // Insert uuid at end of trak (before trak end).
  data.insert(data.begin() + (std::ptrdiff_t)trakEnd, uuidBox.begin(), uuidBox.end());

  // After insert, offsets >= trakEnd shifted by add. Re-find moov (same start if insert inside moov).
  if (!growPathSizes(data, path, add)) {
    if (errorOut) *errorOut = L"failed to update box sizes";
    return false;
  }

  // If moov sits before mdat, chunk offsets must move by `add`.
  BoxLoc moovAfter;
  if (!readBox(data, moovStart, data.size(), moovAfter)) {
    if (errorOut) *errorOut = L"moov broken after inject";
    return false;
  }
  // Re-scan mdat position after insert
  size_t newMdat = 0;
  bool foundMdat = false;
  pos = 0;
  while (pos + 8 <= data.size()) {
    BoxLoc b;
    if (!readBox(data, pos, data.size(), b)) break;
    if (fourccEq((const uint8_t*)b.type, "mdat")) {
      newMdat = b.start;
      foundMdat = true;
      break;
    }
    pos = b.start + b.size;
  }
  if (foundMdat && moovStart < newMdat) {
    // Insert was inside moov before mdat → all mdat-relative chunk offsets increase by add.
    patchChunkOffsets(data, moovStart, moovStart + moovAfter.size, (int64_t)add);
  }

  std::ofstream out(outMp4, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (errorOut) *errorOut = L"cannot write output mp4";
    return false;
  }
  out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
  if (!out.good()) {
    if (errorOut) *errorOut = L"write failed";
    return false;
  }

  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "Spherical MP4 metadata injected (%s, %s, %dx%d)",
                vrMode == VrMode::VR180 ? "VR180" : "VR360",
                stereoMode == StereoMode::Stereo ? "SBS" : "mono",
                frameW, frameH);
  Diagnostics::instance().info(buf);
  return true;
}

} // namespace mmdvr
