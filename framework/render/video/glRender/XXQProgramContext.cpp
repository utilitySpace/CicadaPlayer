#define LOG_TAG "XXQYUVProgramContext"

#include "XXQProgramContext.h"
#include <render/video/glRender/base/utils.h>
#include <utils/AFMediaType.h>
#include <utils/CicadaJSON.h>
#include <utils/CicadaUtils.h>
#include <utils/frame_work_log.h>
#define STB_IMAGE_IMPLEMENTATION
#include "src/OpenGLVideo.h"
#include "stb_image.h"

#ifdef WIN32
#include <gdiplus.h>
#endif// Win32


const GLfloat kColorConversion601FullRange[3][3] = {{1.0, 1.0, 1.0}, {0.0, -0.343, 1.765}, {1.4, -0.711, 0.0}};

static void createTexCoordsArray(int width, int height, Rect pointRect, GLfloat *ret)
{
    ret[0] = (float) pointRect.left / (float) width;
    ret[1] = (float) pointRect.top / (float) height;

    ret[2] = (float) pointRect.right / (float) width;
    ret[3] = (float) pointRect.top / (float) height;

    ret[4] = (float) pointRect.left / (float) width;
    ret[5] = (float) pointRect.bottom / (float) height;

    ret[6] = (float) pointRect.right / (float) width;
    ret[7] = (float) pointRect.bottom / (float) height;
};

static void rotate90(GLfloat *array)
{
    // 0->2 1->0 3->1 2->3
    float tx = array[0];
    float ty = array[1];

    // 1->0
    array[0] = array[2];
    array[1] = array[3];

    // 3->1
    array[2] = array[6];
    array[3] = array[7];

    // 2->3
    array[6] = array[4];
    array[7] = array[5];

    // 0->2
    array[4] = tx;
    array[5] = ty;
}

MixRenderer::MixRenderer(AFPixelFormat format) : mPixelFormat(format)
{
    m_mixVertexShader = R"(
			attribute vec2 a_Position;
			attribute vec2 a_TextureSrcCoordinates;
			attribute vec2 a_TextureMaskCoordinates;
			varying vec2 v_TextureSrcCoordinates;
			varying vec2 v_TextureMaskCoordinates;
			uniform mat4 u_MixMatrix;
			void main()
			{
				v_TextureSrcCoordinates = a_TextureSrcCoordinates;
				v_TextureMaskCoordinates = a_TextureMaskCoordinates;
				gl_Position = u_MixMatrix * vec4(a_Position, 0.0, 1.0) * vec4(1.0, -1.0, 1.0, 1.0);
			}
	)";

    m_mixFragmentShader = mPixelFormat == AF_PIX_FMT_NV12 ?
                                                          R"(
			uniform sampler2D u_TextureMaskUnitY;
			uniform sampler2D u_TextureMaskUnitUV;
			uniform sampler2D u_TextureSrcUnit;
			uniform int u_isFill;
			uniform vec4 u_Color;
			varying vec2 v_TextureSrcCoordinates;
			varying vec2 v_TextureMaskCoordinates;
			uniform mat3 colorConversionMatrix;
			void main()
			{
				vec3 yuv_rgb;
				vec3 rgb_rgb;
				vec4 srcRgba = texture2D(u_TextureSrcUnit, v_TextureSrcCoordinates);

				yuv_rgb.x = (texture2D(u_TextureMaskUnitY, v_TextureMaskCoordinates).r) - 16.0/255.0;
				yuv_rgb.y = (texture2D(u_TextureMaskUnitUV, v_TextureMaskCoordinates).r - 0.5);
				yuv_rgb.z = (texture2D(u_TextureMaskUnitUV, v_TextureMaskCoordinates).a - 0.5);
				rgb_rgb = colorConversionMatrix * yuv_rgb;

				float isFill = step(0.5, float(u_isFill));
				vec4 srcRgbaCal = isFill * vec4(u_Color.r, u_Color.g, u_Color.b, srcRgba.a) + (1.0 - isFill) * srcRgba;
				gl_FragColor = vec4(srcRgbaCal.r, srcRgbaCal.g, srcRgbaCal.b, srcRgba.a * rgb_rgb.r);
			}
	)"
                                                          :
                                                          R"(
			uniform sampler2D u_TextureMaskUnitY;
			uniform sampler2D u_TextureMaskUnitU;
			uniform sampler2D u_TextureMaskUnitV;
			uniform sampler2D u_TextureSrcUnit;
			uniform int u_isFill;
			uniform vec4 u_Color;
			varying vec2 v_TextureSrcCoordinates;
			varying vec2 v_TextureMaskCoordinates;
			uniform mat3 colorConversionMatrix;
			void main()
			{
				vec3 yuv_rgb;
				vec3 rgb_rgb;
				vec4 srcRgba = texture2D(u_TextureSrcUnit, v_TextureSrcCoordinates);

				yuv_rgb.x = (texture2D(u_TextureMaskUnitY, v_TextureMaskCoordinates).r) - 16.0/255.0;
				yuv_rgb.y = (texture2D(u_TextureMaskUnitU, v_TextureMaskCoordinates).r - 0.5);
			    yuv_rgb.z = (texture2D(u_TextureMaskUnitV, v_TextureMaskCoordinates).r - 0.5);
				rgb_rgb = colorConversionMatrix * yuv_rgb;

				float isFill = step(0.5, float(u_isFill));
				vec4 srcRgbaCal = isFill * vec4(u_Color.r, u_Color.g, u_Color.b, srcRgba.a) + (1.0 - isFill) * srcRgba;
				gl_FragColor = vec4(srcRgbaCal.r, srcRgbaCal.g, srcRgbaCal.b, srcRgba.a * rgb_rgb.r);
			}
	)";

    compileMixShader();
}

MixRenderer::~MixRenderer()
{
    clearMixSrc();

    glDisableVertexAttribArray(aPositionLocation);
    glDisableVertexAttribArray(aTextureSrcCoordinatesLocation);
    glDisableVertexAttribArray(aTextureMaskCoordinatesLocation);
    glDetachShader(m_MixshaderProgram, mVertShader);
    glDetachShader(m_MixshaderProgram, mFragmentShader);
    glDeleteShader(mVertShader);
    glDeleteShader(mFragmentShader);
    glDeleteProgram(m_MixshaderProgram);
}

void MixRenderer::compileMixShader()
{
    AF_LOGD("create MixRenderer Program ");

    m_MixshaderProgram = glCreateProgram();
    int mInitRet = IProgramContext::compileShader(&mVertShader, m_mixVertexShader.c_str(), GL_VERTEX_SHADER);

    if (mInitRet != 0) {
        AF_LOGE("compileShader mVertShader failed. ret = %d ", mInitRet);
        return;
    }

    mInitRet = IProgramContext::compileShader(&mFragmentShader, m_mixFragmentShader.c_str(), GL_FRAGMENT_SHADER);

    if (mInitRet != 0) {
        AF_LOGE("compileShader mFragmentShader failed. ret = %d ", mInitRet);
        return;
    }

    glAttachShader(m_MixshaderProgram, mVertShader);
    glAttachShader(m_MixshaderProgram, mFragmentShader);
    glLinkProgram(m_MixshaderProgram);
    GLint status;
    glGetProgramiv(m_MixshaderProgram, GL_LINK_STATUS, &status);

    if (status != GL_TRUE) {
        int length = 0;
        GLchar glchar[256] = {0};
        glGetProgramInfoLog(m_MixshaderProgram, 256, &length, glchar);
        AF_LOGW("linkProgram  error is %s \n", glchar);
        return;
    }

    glUseProgram(m_MixshaderProgram);
    uTextureSrcUnitLocation = glGetUniformLocation(m_MixshaderProgram, "u_TextureSrcUnit");
    uTextureMaskUnitLocationY = glGetUniformLocation(m_MixshaderProgram, "u_TextureMaskUnitY");
    uTextureMaskUnitLocationU = mPixelFormat == AF_PIX_FMT_NV12 ? glGetUniformLocation(m_MixshaderProgram, "u_TextureMaskUnitUV")
                                                                : glGetUniformLocation(m_MixshaderProgram, "u_TextureMaskUnitU");
    uTextureMaskUnitLocationV = glGetUniformLocation(m_MixshaderProgram, "u_TextureMaskUnitV");
    uMixMatrix = glGetUniformLocation(m_MixshaderProgram, "u_MixMatrix");
    uIsFillLocation = glGetUniformLocation(m_MixshaderProgram, "u_isFill");
    uColorLocation = glGetUniformLocation(m_MixshaderProgram, "u_Color");
    mixColorConversionMatrix = glGetUniformLocation(m_MixshaderProgram, "colorConversionMatrix");

    aPositionLocation = glGetAttribLocation(m_MixshaderProgram, "a_Position");
    aTextureSrcCoordinatesLocation = glGetAttribLocation(m_MixshaderProgram, "a_TextureSrcCoordinates");
    aTextureMaskCoordinatesLocation = glGetAttribLocation(m_MixshaderProgram, "a_TextureMaskCoordinates");

    glEnableVertexAttribArray(aPositionLocation);
    glEnableVertexAttribArray(aTextureSrcCoordinatesLocation);
    glEnableVertexAttribArray(aTextureMaskCoordinatesLocation);

    mReady = true;
}

void MixRenderer::renderMix(int index, const MixParam &param)
{
    if (m_allFrames.find(index) == m_allFrames.end()) return;

    const FrameSet &set = m_allFrames[index];
    for (auto iter = set.m_frames.begin(); iter != set.m_frames.end(); iter++) {
        const VAPFrame &frame = iter->second;
        renderMixPrivate(frame, m_srcMap[frame.srcId], param);
    }
}

void MixRenderer::renderMixPrivate(const VAPFrame &frame, const MixSrc &src, const MixParam &param)
{
    auto createVertexArray = [](Rect pointRect, GLfloat *ret, const MixParam &p) {
        if (p.rotate == IVideoRender::Rotate::Rotate_None) {
            float left = pointRect.left * p.scaleW;
            float top = pointRect.top * p.scaleH;
            float right = pointRect.right * p.scaleW;
            float bottom = pointRect.bottom * p.scaleH;

            ret[0] = left + p.offsetX;
            ret[1] = top + p.offsetY;
            ret[2] = right + p.offsetX;
            ret[3] = top + p.offsetY;
            ret[4] = left + p.offsetX;
            ret[5] = bottom + p.offsetY;
            ret[6] = right + p.offsetX;
            ret[7] = bottom + p.offsetY;
        } else if (p.rotate == IVideoRender::Rotate::Rotate_90) {
            float left = pointRect.top * p.scaleW;
            float top = (p.ow - pointRect.right) * p.scaleH;
            float right = pointRect.bottom * p.scaleW;
            float bottom = (p.ow - pointRect.left) * p.scaleH;

            ret[0] = left + p.offsetX;
            ret[1] = bottom + p.offsetY;
            ret[2] = left + p.offsetX;
            ret[3] = top + p.offsetY;
            ret[4] = right + p.offsetX;
            ret[5] = bottom + p.offsetY;
            ret[6] = right + p.offsetX;
            ret[7] = top + p.offsetY;
        } else if (p.rotate == IVideoRender::Rotate::Rotate_180) {
            float left = (p.ow - pointRect.right) * p.scaleW;
            float top = (p.oh - pointRect.bottom) * p.scaleH;
            float right = (p.ow - pointRect.left) * p.scaleW;
            float bottom = (p.oh - pointRect.top) * p.scaleH;

            ret[0] = right + p.offsetX;
            ret[1] = bottom + p.offsetY;
            ret[2] = left + p.offsetX;
            ret[3] = bottom + p.offsetY;
            ret[4] = right + p.offsetX;
            ret[5] = top + p.offsetY;
            ret[6] = left + p.offsetX;
            ret[7] = top + p.offsetY;
        } else if (p.rotate == IVideoRender::Rotate::Rotate_270) {
            float left = (p.oh - pointRect.bottom) * p.scaleW;
            float top = pointRect.left * p.scaleH;
            float right = (p.oh - pointRect.top) * p.scaleW;
            float bottom = pointRect.right * p.scaleH;

            ret[0] = right + p.offsetX;
            ret[1] = top + p.offsetY;
            ret[2] = right + p.offsetX;
            ret[3] = bottom + p.offsetY;
            ret[4] = left + p.offsetX;
            ret[5] = top + p.offsetY;
            ret[6] = left + p.offsetX;
            ret[7] = bottom + p.offsetY;
        }
    };

    if (src.srcTexture == 0) return;

    glUseProgram(m_MixshaderProgram);

    glUniformMatrix3fv(mixColorConversionMatrix, 1, GL_FALSE, (GLfloat *) kColorConversion601FullRange);
    glUniformMatrix4fv(uMixMatrix, 1, GL_FALSE, (GLfloat *) param.matrix);

    createVertexArray(frame.frame, m_mixVertexArray, param);
    glVertexAttribPointer(aPositionLocation, 2, GL_FLOAT, GL_FALSE, 0, m_mixVertexArray);

    genSrcCoordsArray(m_mixSrcArray, frame.frame.width(), frame.frame.height(), src.w, src.h, src.fitType);
    glVertexAttribPointer(aTextureSrcCoordinatesLocation, 2, GL_FLOAT, GL_FALSE, 0, m_mixSrcArray);

    createTexCoordsArray(param.videoW, param.videoH, frame.mFrame, m_maskArray);
    if (frame.mt == 90) rotate90(m_maskArray);

    glVertexAttribPointer(aTextureMaskCoordinatesLocation, 2, GL_FLOAT, GL_FALSE, 0, m_maskArray);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, param.videoTexture[0]);
    glUniform1i(uTextureMaskUnitLocationY, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, param.videoTexture[1]);
    glUniform1i(uTextureMaskUnitLocationU, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, param.videoTexture[2]);
    glUniform1i(uTextureMaskUnitLocationV, 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, src.srcTexture);
    glUniform1i(uTextureSrcUnitLocation, 3);

    if (src.srcType == MixSrc::TXT) {
        glUniform1i(uIsFillLocation, 1);
        glUniform4f(uColorLocation, src.color[0], src.color[1], src.color[2], src.color[3]);
    } else {
        glUniform1i(uIsFillLocation, 0);
        glUniform4f(uColorLocation, 0.f, 0.f, 0.f, 0.f);
    }

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

void MixRenderer::setVapxInfo(const CicadaJSONItem &json)
{
    clearMixSrc();
    CicadaJSONArray arr = json.getItem("src");
    for (int i = 0; i < arr.getSize(); i++) {
        CicadaJSONItem obj = arr.getItem(i);
        m_srcMap.insert({obj.getString("srcId"), MixSrc(obj)});
    }

    m_allFrames.clear();
    CicadaJSONArray frameArr = json.getItem("frame");
    for (int i = 0; i < frameArr.getSize(); i++) {
        FrameSet s(frameArr.getItem(i));
        m_allFrames.insert({s.index, s});
    }

    prepareMixResource();
}

void MixRenderer::setMixResource(const std::string &rcStr)
{
    m_mixResource.clear();

    CicadaJSONItem json(rcStr);
    m_mixResource = json.getStringMap();
}

void MixRenderer::genSrcCoordsArray(GLfloat *array, int fw, int fh, int sw, int sh, MixSrc::FitType fitType)
{
    if (fitType == MixSrc::CENTER_FULL) {
        if (fw <= sw && fh <= sh) {
            // ���Ķ��룬������
            int gw = (sw - fw) / 2;
            int gh = (sh - fh) / 2;
            createTexCoordsArray(sw, sh, Rect(gw, gh, fw + gw, fh + gh), array);
        } else {// centerCrop
            float fScale = fw * 1.0f / fh;
            float sScale = sw * 1.0f / sh;
            Rect srcRect;
            if (fScale > sScale) {
                int w = sw;
                int x = 0;
                int h = (sw / fScale);
                int y = (sh - h) / 2;

                srcRect = Rect(x, y, w + x, h + y);
            } else {
                int h = sh;
                int y = 0;
                int w = (sh * fScale);
                int x = (sw - w) / 2;
                srcRect = Rect(x, y, w + x, h + y);
            }
            createTexCoordsArray(sw, sh, srcRect, array);
        }
    } else {// Ĭ�� fitXY
        createTexCoordsArray(fw, fh, Rect(0, 0, fw, fh), array);
    }
}

void MixRenderer::prepareMixResource()
{
    for (auto iter = m_srcMap.begin(); iter != m_srcMap.end(); iter++) {
        MixSrc &mixSrc = iter->second;
        if (m_mixResource.find(mixSrc.srcTag) == m_mixResource.end()) continue;

        auto resourceStr = m_mixResource[mixSrc.srcTag];
        if (mixSrc.srcType == MixSrc::IMG) {
            if (resourceStr.length() != 0) {
                glGenTextures(1, &mixSrc.srcTexture);
                int w, h, c;
                auto bytes = stbi_load(resourceStr.c_str(), &w, &h, &c, 4);//rgba
                glBindTexture(GL_TEXTURE_2D, mixSrc.srcTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
                stbi_image_free(bytes);
            }
        } else if (mixSrc.srcType == MixSrc::TXT) {
#ifdef WIN32
            Gdiplus::GdiplusStartupInput gdiplusStartupInput;
            ULONG_PTR gdiplusToken;
            Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

            {
                std::unique_ptr<uint8_t> bits(new uint8_t[mixSrc.w * mixSrc.h * 4]);
                auto *bitmap = new Gdiplus::Bitmap(mixSrc.w, mixSrc.h, 4 * mixSrc.w, PixelFormat32bppARGB, bits.get());
                auto *graphics = new Gdiplus::Graphics(bitmap);

                graphics->Clear(Gdiplus::Color::Transparent);

                int fs = 65;
                int style = mixSrc.style == MixSrc::BOLD ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;
                std::wstring text = CicadaUtils::StringToWideString(resourceStr);
                Gdiplus::FontFamily fm(L"Microsoft YaHei");

                Gdiplus::StringFormat strformat(Gdiplus::StringFormat::GenericTypographic());
                strformat.SetAlignment(Gdiplus::StringAlignmentCenter);    //ˮƽ����
                strformat.SetLineAlignment(Gdiplus::StringAlignmentCenter);//��ֱ����
                graphics->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
                graphics->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                while (true) {
                    Gdiplus::RectF boundingBox;
                    Gdiplus::Font font(&fm, --fs, style, Gdiplus::UnitPixel);
                    graphics->MeasureString(text.c_str(), (int) text.size() + 1, &font, Gdiplus::PointF(0.0f, 0.0f), &strformat,
                                            &boundingBox);
                    if (boundingBox.Width <= (float) mixSrc.w && boundingBox.Height <= (float) mixSrc.h) break;

                    if (fs <= 1) break;
                }

                auto brush = new Gdiplus::SolidBrush(
                        Gdiplus::Color(mixSrc.color[3] * 255, mixSrc.color[0] * 255, mixSrc.color[1] * 255, mixSrc.color[2] * 255));
                auto pfont = new Gdiplus::Font(&fm, fs, style, Gdiplus::UnitPixel);
                graphics->DrawString(text.c_str(), -1, pfont, Gdiplus::RectF(0, 0, mixSrc.w, mixSrc.h), &strformat, brush);

                glGenTextures(1, &mixSrc.srcTexture);
                glBindTexture(GL_TEXTURE_2D, mixSrc.srcTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mixSrc.w, mixSrc.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bits.get());

                delete pfont;
                delete bitmap;
                delete graphics;
                delete brush;
            }

            Gdiplus::GdiplusShutdown(gdiplusToken);
#endif
        }
    }
}

void MixRenderer::clearMixSrc()
{
    for (auto iter = m_srcMap.begin(); iter != m_srcMap.end(); iter++) {
        MixSrc &mixSrc = iter->second;
        if (mixSrc.srcTexture != 0) {
            glDeleteTextures(1, &mixSrc.srcTexture);
        }
    }
    m_srcMap.clear();
}

static const char YUV_VERTEX_SHADER[] = R"(
        attribute vec2 a_position;
        attribute vec2 a_texCoord;
		attribute vec2 a_alpha_texCoord;
        uniform mat4 u_projection;
		uniform vec2 u_flipMatrix;
        varying vec2 v_texCoord;
		varying vec2 v_alpha_texCoord;

        void main() {
            gl_Position = u_projection * vec4(a_position, 0.0, 1.0) * vec4(u_flipMatrix, 1.0, 1.0) * vec4(1.0, -1.0, 1.0, 1.0);
            v_texCoord  = a_texCoord;
			v_alpha_texCoord = a_alpha_texCoord;
        }
)";

static const char YUV_FRAGMENT_SHADER[] = R"(
#ifdef GL_ES
        precision mediump float;
#endif
        uniform sampler2D y_tex;
        uniform sampler2D u_tex;
        uniform sampler2D v_tex;

        uniform mat3      uColorSpace;
        uniform vec3      uColorRange;
		uniform int		  uAlphaBlend;

        varying vec2 v_texCoord;
		varying vec2 v_alpha_texCoord;

        void main() {
            vec3 yuv;
            vec3 rgb;
			vec3 yuv_alpha;
            vec3 rgb_alpha;
            yuv.x = (texture2D(y_tex, v_texCoord).r - uColorRange.x / 255.0) * 255.0 / uColorRange.y;
            yuv.y = (texture2D(u_tex, v_texCoord).r - 0.5) * 255.0 / uColorRange.z;
            yuv.z = (texture2D(v_tex, v_texCoord).r - 0.5) * 255.0 / uColorRange.z;
            rgb = uColorSpace * yuv;

			if (uAlphaBlend == 1) {
				yuv_alpha.x = (texture2D(y_tex, v_alpha_texCoord).r - uColorRange.x / 255.0) * 255.0 / uColorRange.y;
				yuv_alpha.y = (texture2D(u_tex, v_alpha_texCoord).r - 0.5) * 255.0 / uColorRange.z;
				yuv_alpha.z = (texture2D(v_tex, v_alpha_texCoord).r - 0.5) * 255.0 / uColorRange.z;
				rgb_alpha = uColorSpace * yuv_alpha;

				gl_FragColor = vec4(rgb, rgb_alpha.x);
			} else 
				gl_FragColor = vec4(rgb, 1);
			}
)";

static const char NV12_YUV_FRAGMENT_SHADER[] = R"(
#ifdef GL_ES
        precision mediump float;
#endif
        uniform sampler2D y_tex;
        uniform sampler2D uv_tex;

        uniform mat3      uColorSpace;
        uniform vec3      uColorRange;
		uniform int		  uAlphaBlend;

        varying vec2 v_texCoord;
		varying vec2 v_alpha_texCoord;

        void main() {
            vec3 yuv;
            vec3 rgb;
			vec3 yuv_alpha;
            vec3 rgb_alpha;
            yuv.x = (texture2D(y_tex, v_texCoord).r - uColorRange.x / 255.0) * 255.0 / uColorRange.y;
            yuv.y = (texture2D(uv_tex, v_texCoord).r - 0.5) * 255.0 / uColorRange.z;
			yuv.z = (texture2D(uv_tex, v_texCoord).a - 0.5) * 255.0 / uColorRange.z;
            rgb = uColorSpace * yuv;

			if (uAlphaBlend == 1) {
				yuv_alpha.x = (texture2D(y_tex, v_alpha_texCoord).r - uColorRange.x / 255.0) * 255.0 / uColorRange.y;
				yuv_alpha.y = (texture2D(uv_tex, v_alpha_texCoord).r - 0.5) * 255.0 / uColorRange.z;
				yuv_alpha.z = (texture2D(uv_tex, v_alpha_texCoord).a - 0.5) * 255.0 / uColorRange.z;
				rgb_alpha = uColorSpace * yuv_alpha;

				gl_FragColor = vec4(rgb, rgb_alpha.x);
			} else 
				gl_FragColor = vec4(rgb, 1);
			}
)";

XXQYUVProgramContext::XXQYUVProgramContext() : mOpenGL(new OpenGLVideo), mWindowWidth(0), mWindowHeight(0), mFrameWidth(0), mFrameHeight(0)
{
    AF_LOGD("YUVProgramContext");
}

XXQYUVProgramContext::~XXQYUVProgramContext()
{
    AF_LOGD("~YUVProgramContext");

    delete mOpenGL;
}

void XXQYUVProgramContext::updateScale(IVideoRender::Scale scale)
{
    if (mScale != scale) {
        mScale = scale;
        mDrawRectChanged = true;
    }
}

void XXQYUVProgramContext::updateRotate(IVideoRender::Rotate rotate)
{
    if (mRotate != rotate) {
        mRotate = rotate;
        mDrawRectChanged = true;
    }
}

void XXQYUVProgramContext::updateFlip(IVideoRender::Flip flip)
{
    if (mFlip != flip) {
        mFlip = flip;
        mDrawRectChanged = true;
    }
}

void XXQYUVProgramContext::updateMaskInfo(const std::string &vapInfo, IVideoRender::MaskMode mode, const std::string &data)
{
    if (mVapInfo != vapInfo || mMode != mode || mMaskVapData != data) {
        mVapInfo = vapInfo;
        mMode = mode;
        mMaskVapData = data;
        mMaskInfoChanged = true;
    }
}

int XXQYUVProgramContext::updateFrame(std::unique_ptr<IAFFrame> &frame)
{
    if (frame != nullptr) {
        IAFFrame::videoInfo &videoInfo = frame->getInfo().video;
        if (mFrameWidth != videoInfo.width || mFrameHeight != videoInfo.height) {
            mFrameWidth = videoInfo.width;
            mFrameHeight = videoInfo.height;
            mDrawRectChanged = true;
        }
    }

    if (frame == nullptr && !mDrawRectChanged && !mBackgroundColorChanged) {
        //frame is null and nothing changed , don`t need redraw. such as paused.
        return -1;
    }

    bool rendered = false;
    if (mRenderingCb) {
        CicadaJSONItem params{};
        rendered = mRenderingCb(mRenderingCbUserData, frame.get(), params);
    }

    if (rendered) {
        return -1;
    }

    if (mDrawRectChanged) {
        auto setOutAspectRatio = [this](double outAspectRatio) {
            double rendererAspectRatio = double(mWindowWidth) / double(mWindowHeight);
            if (mScale == IVideoRender::Scale_Fill) {
                return QRect(0, 0, mWindowWidth, mWindowHeight);
            }
            // dar: displayed aspect ratio in video renderer orientation

            QRect out_rect;
            int rotate = mRotate;
            const double dar = (rotate % 180) ? 1.0 / outAspectRatio : outAspectRatio;
            if (rendererAspectRatio >= dar) {//equals to original video aspect ratio here, also equals to out ratio
                //renderer is too wide, use renderer's height, horizonal align center
                const int h = mWindowHeight;
                const int w = std::round(dar * double(h));
                out_rect = QRect((mWindowWidth - w) / 2, 0, w, h);
            } else if (rendererAspectRatio < dar) {
                //renderer is too high, use renderer's width
                const int w = mWindowWidth;
                const int h = std::round(double(w) / dar);
                out_rect = QRect(0, (mWindowHeight - h) / 2, w, h);
            }
            return out_rect;
        };

        QRect outRect;
        switch (mScale) {
            case IVideoRender::Scale_AspectFit:
                outRect = setOutAspectRatio((double) mFrameWidth / (double) mFrameHeight);
                break;
            case IVideoRender::Scale_AspectFill:
                break;
            case IVideoRender::Scale_Fill:
                outRect = setOutAspectRatio(double(mWindowWidth) / double(mWindowHeight));
                break;
            default:
                break;
        }

        matrix.setToIdentity();
        matrix.scale((GLfloat) outRect.width() / (GLfloat) mWindowWidth, (GLfloat) outRect.height() / (GLfloat) mWindowHeight, 1);
        switch (mFlip) {
            case IVideoRender::Flip_None:
                break;
            case IVideoRender::Flip_Horizontal:
                matrix.scale(-1.0f, 1.0f);
                break;
            case IVideoRender::Flip_Vertical:
                matrix.scale(1.0f, -1.0f);
                break;
            case IVideoRender::Flip_Both:
                matrix.scale(-1.0f, -1.0f);
                break;
            default:
                break;
        }
        if (mRotate) matrix.rotate(mRotate, 0, 0, 1);// Z axis

        mDrawRectChanged = false;
    }

    mOpenGL->setViewport(QRectF(0, 0, mWindowWidth, mWindowHeight));
    if (mBackgroundColorChanged) {
        mOpenGL->fill(mBackgroundColor);
        mBackgroundColorChanged = false;
    }
    mOpenGL->render(frame, QRectF(), QRectF(0, 0, mFrameWidth, mFrameHeight), matrix);

    return 0;
}

void XXQYUVProgramContext::updateVapConfig()
{
    mVapConfig = std::make_unique<VapAnimateConfig>();

    if (mVapInfo.length() == 0) {
        mVapConfig->videoWidth = mFrameWidth;
        mVapConfig->videoHeight = mFrameHeight;
        switch (mMode) {
            case IVideoRender::Mask_None:
                mVapConfig->width = mFrameWidth;
                mVapConfig->height = mFrameHeight;
                mVapConfig->rgbPointRect = Rect(0, 0, mFrameWidth, mFrameHeight);
                break;
            case IVideoRender::Mask_Left:
                mVapConfig->width = mFrameWidth / 2;
                mVapConfig->height = mFrameHeight;
                mVapConfig->alphaPointRect = Rect(0, 0, mFrameWidth / 2, mFrameHeight);
                mVapConfig->rgbPointRect = Rect(mFrameWidth / 2, 0, mFrameWidth, mFrameHeight);
                break;
            case IVideoRender::Mask_Right:
                mVapConfig->width = mFrameWidth / 2;
                mVapConfig->height = mFrameHeight;
                mVapConfig->alphaPointRect = Rect(mFrameWidth / 2, 0, mFrameWidth, mFrameHeight);
                mVapConfig->rgbPointRect = Rect(0, 0, mFrameWidth / 2, mFrameHeight);
                break;
            case IVideoRender::Mask_Top:
                mVapConfig->width = mFrameWidth;
                mVapConfig->height = mFrameHeight / 2;
                mVapConfig->alphaPointRect = Rect(0, 0, mFrameWidth, mFrameHeight / 2);
                mVapConfig->rgbPointRect = Rect(0, mFrameHeight / 2, mFrameWidth, mFrameHeight);
                break;
            case IVideoRender::Mask_Down:
                mVapConfig->width = mFrameWidth;
                mVapConfig->height = mFrameHeight / 2;
                mVapConfig->alphaPointRect = Rect(0, mFrameHeight / 2, mFrameWidth, mFrameHeight);
                mVapConfig->rgbPointRect = Rect(0, 0, mFrameWidth, mFrameHeight / 2);
                break;
            default:
                break;
        }

        return;
    }

    CicadaJSONItem json(mVapInfo);
    CicadaJSONItem info = json.getItem("info");
    mVapConfig->version = info.getInt("v", 0);
    mVapConfig->totalFrames = info.getInt("f", 0);
    mVapConfig->width = info.getInt("w", 0);
    mVapConfig->height = info.getInt("h", 0);
    mVapConfig->videoWidth = info.getInt("videoW", 0);
    mVapConfig->videoHeight = info.getInt("videoH", 0);
    mVapConfig->orien = info.getInt("orien", 0);
    mVapConfig->fps = info.getInt("fps", 0);
    mVapConfig->isMix = info.getInt("isVapx", 0) != 0;

    CicadaJSONArray rgbArray = info.getItem("rgbFrame");
    assert(rgbArray.getSize() == 4);
    mVapConfig->rgbPointRect =
            Rect(rgbArray.getItem(0).getInt(), rgbArray.getItem(1).getInt(), rgbArray.getItem(0).getInt() + rgbArray.getItem(2).getInt(),
                 rgbArray.getItem(1).getInt() + rgbArray.getItem(3).getInt());

    CicadaJSONArray alphaArray = info.getItem("aFrame");
    assert(alphaArray.getSize() == 4);
    mVapConfig->alphaPointRect = Rect(alphaArray.getItem(0).getInt(), alphaArray.getItem(1).getInt(),
                                      alphaArray.getItem(0).getInt() + alphaArray.getItem(2).getInt(),
                                      alphaArray.getItem(1).getInt() + alphaArray.getItem(3).getInt());

    //mMixRender = std::make_unique<MixRenderer>(mPixelFormat);
    mMixRender->setMixResource(mMaskVapData);
    mMixRender->setVapxInfo(json);
}

void XXQYUVProgramContext::updateWindowSize(int width, int height, bool windowChanged)
{
    (void) windowChanged;
    if (mWindowWidth == width && mWindowHeight == height) {
        return;
    }

    mWindowWidth = width;
    mWindowHeight = height;

    mDrawRectChanged = true;
}

void XXQYUVProgramContext::updateBackgroundColor(uint32_t color)
{
    if (color != mBackgroundColor) {
        mBackgroundColorChanged = true;
        mBackgroundColor = color;
    }
}

void XXQYUVProgramContext::clearScreen(uint32_t color)
{
	mOpenGL->fill(color);
}