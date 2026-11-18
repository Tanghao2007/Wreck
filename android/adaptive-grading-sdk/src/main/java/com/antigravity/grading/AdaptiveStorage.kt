package com.antigravity.grading

/**
 * 调色配方与预设持久化实体
 */
data class StylePresetEntity(
    val id: Long = 0,
    val styleId: Int,
    val presetName: String,
    val author: String = "User",
    val contrast: Float,
    val exposureBias: Float,
    val saturation: Float,
    val blackLift: Float,
    val whiteClip: Float,
    val toeStrength: Float,
    val shoulderStrength: Float,
    val grainIntensity: Float,
    val vignette: Float,
    val isCustom: Boolean = true,
    val createdAt: Long = System.currentTimeMillis()
)

/**
 * 成片拍摄与调色元数据记录实体 (用于与 Android MediaStore / 相册关联)
 */
data class PhotoGradingRecord(
    val photoId: String,
    val filePath: String,
    val appliedStyleId: Int,
    val sceneLabel: String,
    val estimatedCct: Float,
    val dynamicRangeEv: Float,
    val captureTimeMs: Long = System.currentTimeMillis()
)

/**
 * Android 本地数据库 Schema 与迁移规范 (SQLite / Room 规范)
 */
object DatabaseContract {
    const val DATABASE_NAME = "adaptive_grading.db"
    const val DATABASE_VERSION = 1

    object StylePresetsTable {
        const val TABLE_NAME = "style_presets"
        const val COL_ID = "id"
        const val COL_STYLE_ID = "style_id"
        const val COL_PRESET_NAME = "preset_name"
        const val COL_AUTHOR = "author"
        const val COL_CONTRAST = "contrast"
        const val COL_EXPOSURE_BIAS = "exposure_bias"
        const val COL_SATURATION = "saturation"
        const val COL_BLACK_LIFT = "black_lift"
        const val COL_WHITE_CLIP = "white_clip"
        const val COL_TOE_STRENGTH = "toe_strength"
        const val COL_SHOULDER_STRENGTH = "shoulder_strength"
        const val COL_GRAIN_INTENSITY = "grain_intensity"
        const val COL_VIGNETTE = "vignette"
        const val COL_IS_CUSTOM = "is_custom"
        const val COL_CREATED_AT = "created_at"

        const val SQL_CREATE = """
            CREATE TABLE IF NOT EXISTS $TABLE_NAME (
                $COL_ID INTEGER PRIMARY KEY AUTOINCREMENT,
                $COL_STYLE_ID INTEGER NOT NULL,
                $COL_PRESET_NAME TEXT NOT NULL,
                $COL_AUTHOR TEXT DEFAULT 'User',
                $COL_CONTRAST REAL NOT NULL,
                $COL_EXPOSURE_BIAS REAL NOT NULL,
                $COL_SATURATION REAL NOT NULL,
                $COL_BLACK_LIFT REAL NOT NULL,
                $COL_WHITE_CLIP REAL NOT NULL,
                $COL_TOE_STRENGTH REAL NOT NULL,
                $COL_SHOULDER_STRENGTH REAL NOT NULL,
                $COL_GRAIN_INTENSITY REAL NOT NULL,
                $COL_VIGNETTE REAL NOT NULL,
                $COL_IS_CUSTOM INTEGER DEFAULT 1,
                $COL_CREATED_AT INTEGER NOT NULL
            );
        """
    }

    object PhotoHistoryTable {
        const val TABLE_NAME = "photo_grading_history"
        const val COL_PHOTO_ID = "photo_id"
        const val COL_FILE_PATH = "file_path"
        const val COL_APPLIED_STYLE = "applied_style_id"
        const val COL_SCENE_LABEL = "scene_label"
        const val COL_CCT = "estimated_cct"
        const val COL_DR_EV = "dynamic_range_ev"
        const val COL_CAPTURE_TIME = "capture_time"

        const val SQL_CREATE = """
            CREATE TABLE IF NOT EXISTS $TABLE_NAME (
                $COL_PHOTO_ID TEXT PRIMARY KEY,
                $COL_FILE_PATH TEXT NOT NULL,
                $COL_APPLIED_STYLE INTEGER NOT NULL,
                $COL_SCENE_LABEL TEXT,
                $COL_CCT REAL,
                $COL_DR_EV REAL,
                $COL_CAPTURE_TIME INTEGER NOT NULL
            );
        """
    }
}

