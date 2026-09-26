# -*- mode: python -*-

# To use as: pyinstaller samples/pysuperluxcoretool/pysuperluxcoretool.win.spec

block_cipher = None

def binaries_list():
	result = [
		('../../../WindowsCompile/Build_CMake/LuxCore/lib/Release/pysuperluxcore.pyd', '.'),
		('../../../WindowsCompile/Build_CMake/LuxCore/lib/pysuperluxcoretools.zip', '.'),
		('../../../WindowsCompileDeps/x64/Release/lib/OpenImageDenoise.dll', '.'),
		('../../../WindowsCompileDeps/x64/Release/lib/embree3.dll', '.'),
		('../../../WindowsCompileDeps/x64/Release/lib/tbb12.dll', '.'),
        ('../../../WindowsCompileDeps/x64/Release/lib/tbb.dll', '.'),
		('../../../WindowsCompileDeps/x64/Release/lib/tbbmalloc.dll', '.'),
		('../../../WindowsCompileDeps/x64/Release/lib/nvrtc64_101_0.dll', '.'),
		('../../../WindowsCompileDeps/x64/Release/lib/nvrtc-builtins64_101.dll', '.')
	]
	return result

def pyside_imports():
    try:
        import PySide.QtCore as QtCore
        result = ['PySide.QtCore','PySide.QtGui']
    except ImportError:
        try:
            from PySide2 import QtCore
            result = ['PySide2.QtCore','PySide2.QtGui', 'PySide2.QtWidgets']
        except ImportError:
            from PySide6 import QtCore
            result = ['PySide6.QtCore','PySide6.QtGui', 'PySide6.QtWidgets']
    return result

a = Analysis(['pysuperluxcoretool.py'],
             pathex=['../..'],
             binaries=binaries_list(),
             datas=[],
             hiddenimports=['uuid', 'numpy'] + pyside_imports(),
             hookspath=[],
             runtime_hooks=[],
             excludes=[],
             win_no_prefer_redirects=False,
             win_private_assemblies=False,
             cipher=block_cipher)
pyz = PYZ(a.pure, a.zipped_data,
             cipher=block_cipher)
exe = EXE(pyz,
          a.scripts,
          a.binaries,
          a.zipfiles,
          a.datas,
          name='pysuperluxcoretool',
          debug=False,
          strip=False,
          upx=True,
          console=True )
