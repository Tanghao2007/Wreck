# ==============================================================================
# Adaptive Grading SDK - Consumer ProGuard / R8 Rules
# ==============================================================================

# 保留 Native 接口类及所有 native 方法
-keepclassmembers class com.antigravity.grading.AdaptiveGradingEngine {
    private long nativeHandle;
    private native *** native* (...);
}

-keepclassmembers class com.antigravity.grading.AdaptiveStorage {
    private native *** native* (...);
}

# 保留数据传递模型结构体字段 (防止反射序列化失效)
-keep class com.antigravity.grading.Models$* { *; }
-keep class com.antigravity.grading.SceneMetrics { *; }
-keep class com.antigravity.grading.StyleParameters { *; }
-keep class com.antigravity.grading.LoggerSummary { *; }
-keep class com.antigravity.grading.RecipeItem { *; }
-keep class com.antigravity.grading.StyleInfo { *; }

# 保留自定义 UI 视图组件 (支持在 XML 布局文件中声明引用)
-keep class com.antigravity.grading.ui.CameraPreviewView { *; }
-keep class com.antigravity.grading.ui.HUDOverlayView { *; }
-keep class com.antigravity.grading.ui.SplitScreenSlider { *; }
-keep class com.antigravity.grading.ui.StyleSelectorView { *; }

# 忽略 JNI 运行时方法警告
-dontwarn com.antigravity.grading.**

