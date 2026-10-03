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

  PathVolumeInfo: 44 bytes.

Usage:
  python3.13 taskstate_parse.py taskstate_vol_0.bin
"""
import struct, sys, collections, os

def main(path):
    data = open(path, "rb").read()
    pos = 0
    # Records are appended per batch; each starts with taskCount.
    rec = 0
    EPI, PVI = 200, 44   # sizeof(slg::ocl::EyePathInfo/PathVolumeInfo)
    while pos < len(data):
        if pos + 4 > len(data):
            break
        (tc,) = struct.unpack_from("<I", data, pos); pos += 4
        need = tc * (EPI + PVI)
        if pos + need > len(data):
            print(f"truncated record {rec}: tc={tc} need={need}")
            break
        epi_off = pos
        pvi_off = pos + tc * EPI
        # Per-task: volume.currentVolumeIndex, volumeIndexListSize,
        # list, scatteredStart, plus light dlVolInfo.currentVolumeIndex.
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
        pos += need
        rec += 1

if __name__ == "__main__":
    main(sys.argv[1])
