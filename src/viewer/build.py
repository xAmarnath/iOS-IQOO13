"""Build the viewer and bounded scanout/touch bridges with the installed SDK/NDK."""
from pathlib import Path
import os, subprocess, zipfile
here=Path(__file__).resolve().parent
out=here/'build'; out.mkdir(exist_ok=True)
sdk=Path(os.environ['ANDROID_HOME'])
bt=sdk/'build-tools/37.0.0'; android=sdk/'platforms/android-37.0/android.jar'
ndk=sdk/'ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin'
def run(*args): subprocess.run([str(x) for x in args],check=True)
run(ndk/'clang.exe','--target=aarch64-linux-android30','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',here/'stream-native.c','-lGLESv2','-llog','-Wl,-z,max-page-size=16384','-o',out/'libgueststream.so')
run(ndk/'clang.exe','--target=aarch64-linux-android30','-O2','-Wall','-Wextra','-Werror',here/'relay.c','-o',out/'scanout-relay')
run(ndk/'clang.exe','--target=aarch64-linux-android30','-O2','-Wall','-Wextra','-Werror',here/'publish.c','-o',out/'scanout-publish')
for name in ('touch-relay','touch-bridge'):
    run(ndk/'clang.exe','--target=aarch64-linux-android30','-O2','-Wall','-Wextra','-Werror',here/(name+'.c'),'-o',out/name)
classes=out/'classes'; classes.mkdir(exist_ok=True)
run('javac','-source','8','-target','8','-cp',android,'-d',classes,*here.glob('src/org/openoos/vmviewer/*.java'))
run(bt/'aapt2.exe','link','-I',android,'--manifest',here/'AndroidManifest.xml','-o',out/'unsigned.apk')
run(bt/'d8.bat','--lib',android,'--min-api','30','--output',out,*classes.rglob('*.class'))
with zipfile.ZipFile(out/'unsigned.apk','a') as z:
    z.write(out/'classes.dex','classes.dex')
    z.write(out/'libgueststream.so','lib/arm64-v8a/libgueststream.so')
run(bt/'zipalign.exe','-f','4',out/'unsigned.apk',out/'aligned.apk')
key=out/'viewer-local.jks'
if not key.exists():
    run('keytool','-genkeypair','-keystore',key,'-storepass','viewer-local','-keypass','viewer-local','-alias','viewer','-keyalg','RSA','-keysize','3072','-validity','3650','-dname','CN=Local VM Viewer')
run(bt/'apksigner.bat','sign','--ks',key,'--ks-pass','pass:viewer-local','--out',out/'iPadOS-Display.apk',out/'aligned.apk')
run(bt/'apksigner.bat','verify',out/'iPadOS-Display.apk')
print(out/'iPadOS-Display.apk')
