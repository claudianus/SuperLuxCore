#!/usr/bin/env python3.13
"""Parse taskstate_vol_<t>.bin written by LUX_TASKSTATE_VOL_DUMP.

Layout (little-endian, matching slg::ocl::EyePathInfo /
PathVolumeInfo under SLG's device struct compile):

  per record:
    u32 taskCount
    taskCount * EyePathInfo
    taskCount * PathVolumeInfo   (directLightVolInfos)

  EyePathInfo:
    PathDepthInfo depth        offset  0, 32 bytes
      u32 depth, diffuseDepth, glossyDepth, specularDepth,
          transmitDepth, transparentDepth; f32 regularization;
          u32 regularizationMinDepth
    PathVolumeInfo volume      offset 32, 44 bytes
      u32 currentVolumeIndex
      u32 volumeIndexList[8]
      u32 volumeIndexListSize
      i32 scatteredStart
    tail (isPassThroughPath, lastBSDFEvent, ... lpeStates[8])
    -> EyePathInfo total = 32 + 44 + 44 + 8*4 = 152 bytes (verify
       against sizeof via a host program if the tail drifts)

# taskstate_light_<t>.bin records: u32 lightTaskCount + ltCount *
# LightPathInfo (340 B). Volume offsets inside LightPathInfo:
#   depth(32) -> volume(32 @32) -> connectVolInfo(44 @76)
# pendingSplat.valid lives at offset 336 (see offsetof dump).

  PathVolumeInfo: 44 bytes.

Usage:
  python3.13 taskstate_parse.py taskstate_vol_0.bin
"""
import struct, sys, collections, os

def main_eye(path):
    data = open(path, "rb").read()
    rec, pos = 0, 0
    while pos < len(data):
        if pos + 4 > len(data):
            break
        tc, epi_sz, pvi_sz = struct.unpack_from("<III", data, pos)
        pos += 12
        need = tc * (epi_sz + pvi_sz)
        epi_off = pos
        pvi_off = pos + tc * epi_sz
        pos += need

        vol_cur = collections.Counter()
        vol_list = collections.Counter()
        scat = 0
        dl_cur = collections.Counter()
        for i in range(tc):
            b = epi_off + i * EPI
            cv, = struct.unpack_from("<I", data, b + 32)
            sz, = struct.unpack_from("<I", data, b + 32 + 36)
            ss, = struct.unpack_from("<i", data, b + 32 + 40)
            lst = struct.unpack_from("<8I", data, b + 32 + 4)
            vol_cur[cv] += 1
            vol_list[sz] += 1
            scat += (ss != 0)
            b = pvi_off + i * PVI
            dcv, = struct.unpack_from("<I", data, b)
            dl_cur[dcv] += 1
        print(f"--- record {rec} tc={tc}")
        print(f"  eye currentVolumeIndex: {dict(vol_cur)}")
        print(f"  eye listSize dist:      {dict(vol_list)}")
        print(f"  eye scatteredStart=1:   {scat}")
        print(f"  dlVol currentVolume:    {dict(dl_cur)}")
        rec += 1

# taskstate_light_<t>.bin records: u32 lightTaskCount + ltCount *
# LightPathInfo (340 B). Offsets inside LightPathInfo:
#   depth(32) @0 -> volume(44) @32 -> connectVolInfo(44) @76
#   pendingSplat.valid i32 @336 (end of record).
LPI_SIZE = 340
LPI_VOLUME = 32      # PathVolumeInfo volume (current state)
LPI_CONN = 76        # PathVolumeInfo connectVolInfo (pre-march state)
LPI_VALID = 336      # pendingSplat.valid

def main_light(path):
    data = open(path, "rb").read()
    rec, pos = 0, 0
    while pos + 4 <= len(data):
        lt, = struct.unpack_from("<I", data, pos)
        pos += 4
        need = lt * LPI_SIZE
        if pos + need > len(data):
            break
        vol_cur = collections.Counter()
        conn_cur = collections.Counter()
        splat = 0
        for i in range(lt):
            b = pos + i * LPI_SIZE
            cv, = struct.unpack_from("<I", data, b + LPI_VOLUME)
            ccv, = struct.unpack_from("<I", data, b + LPI_CONN)
            v, = struct.unpack_from("<i", data, b + LPI_VALID)
            vol_cur[cv] += 1
            conn_cur[ccv] += 1
            splat += (v != 0)
        print(f"--- record {rec} lt={lt}")
        print(f"  light volume.currentVol:      {dict(vol_cur)}")
        print(f"  light connectVolInfo.curVol:  {dict(conn_cur)}")
        print(f"  light pendingSplat.valid=1:   {splat}")
        pos += need
        rec += 1

def main(path):
    if "taskstate_light" in path:
        main_light(path)
    else:
        main_eye(path)

if __name__ == "__main__":
    main(sys.argv[1])
