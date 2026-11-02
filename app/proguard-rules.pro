# JNI entry points are looked up by name from native code.
-keepclasseswithmembernames class com.paintengine.engine.NativeEngine {
    native <methods>;
}
