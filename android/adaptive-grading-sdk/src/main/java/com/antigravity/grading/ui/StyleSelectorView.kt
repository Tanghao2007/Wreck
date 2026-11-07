package com.antigravity.grading.ui

import android.content.Context
import android.graphics.Color
import android.util.AttributeSet
import android.view.Gravity
import android.view.View
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import com.antigravity.grading.STYLE_CATALOG
import com.antigravity.grading.StyleInfo

/**
 * 底部 15 款电影胶片风格横向选择器 (Style Selector Carousel)
 */
class StyleSelectorView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : HorizontalScrollView(context, attrs, defStyleAttr) {

    private val container = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setPadding(32, 16, 32, 16)
    }

    private var selectedIndex = 0
    var onStyleSelected: ((StyleInfo) -> Unit)? = null

    init {
        isHorizontalScrollBarEnabled = false
        addView(container)
        populateStyles()
    }

    private fun populateStyles() {
        container.removeAllViews()
        for ((idx, style) in STYLE_CATALOG.withIndex()) {
            val itemView = createStyleCard(style, idx == selectedIndex)
            itemView.setOnClickListener {
                selectIndex(idx)
            }
            container.addView(itemView)
        }
    }

    private fun createStyleCard(style: StyleInfo, isSelected: Boolean): View {
        return LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(36, 18, 36, 18)
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                setMargins(14, 0, 14, 0)
            }
            layoutParams = lp

            val shape = android.graphics.drawable.GradientDrawable().apply {
                cornerRadius = 28f
                if (isSelected) {
                    setColor(Color.argb(220, 35, 45, 60))
                    setStroke(4, style.colorHex.toInt())
                } else {
                    setColor(Color.argb(160, 25, 25, 30))
                    setStroke(1, Color.argb(60, 255, 255, 255))
                }
            }
            background = shape

            // 风格标题
            addView(TextView(context).apply {
                text = "${style.id}. ${style.name}"
                textSize = 15f
                setTextColor(if (isSelected) style.colorHex.toInt() else Color.WHITE)
                paint.isFakeBoldText = isSelected
            })

            // 英文标签 / 类别
            addView(TextView(context).apply {
                text = "${style.category} · ${style.englishName}"
                textSize = 11f
                setTextColor(Color.rgb(170, 175, 185))
                setPadding(0, 4, 0, 0)
            })
        }
    }

    fun selectIndex(index: Int) {
        if (index !in STYLE_CATALOG.indices) return
        selectedIndex = index
        populateStyles()

        val selectedStyle = STYLE_CATALOG[index]
        onStyleSelected?.invoke(selectedStyle)

        // 自动平滑滚动到居中
        post {
            val child = container.getChildAt(index) ?: return@post
            val targetScrollX = child.left - (width - child.width) / 2
            smoothScrollTo(Math.max(0, targetScrollX), 0)
        }
    }

    fun getSelectedStyle(): StyleInfo = STYLE_CATALOG[selectedIndex]
}
