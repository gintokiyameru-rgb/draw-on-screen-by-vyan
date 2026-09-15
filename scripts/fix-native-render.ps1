$ErrorActionPreference = 'Stop'

$path = Join-Path $env:GITHUB_WORKSPACE 'obs-template\src\plugin-main.cpp'
if (-not (Test-Path $path)) { throw "plugin-main.cpp not found: $path" }

$text = Get-Content $path -Raw
$pattern = '(?s)static void sourceVideoRender\(void \*data, gs_effect_t \*effect\)\n\{.*?\n\}\n\nstatic obs_source_info g_sourceInfoInit\(\)'
$replacement = @'
static void sourceVideoRender(void *data, gs_effect_t *effect)
{
    Q_UNUSED(data);
    if (!g_visible || !effect)
        return;

    QImage composite;
    uint64_t revision = 0;
    {
        QMutexLocker lock(&g_docMutex);
        ensureDocument();
        composite = compositeDocument();
        revision = g_revision;
    }

    if (!g_texture) {
        g_texture = gs_texture_create(kCanvasW, kCanvasH, GS_RGBA, 1, nullptr, GS_DYNAMIC);
        if (!g_texture) {
            blog(LOG_WARNING, "[VyanHQ Draw] failed to create native canvas texture");
            return;
        }
        g_textureRevision = 0;
    }

    if (revision != g_textureRevision) {
        QImage rgba = composite.convertToFormat(QImage::Format_RGBA8888);
        gs_texture_set_image(g_texture, rgba.constBits(), (uint32_t)rgba.bytesPerLine(), false);
        g_textureRevision = revision;
    }

    const bool previousSrgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    gs_eparam_t *const imageParam = gs_effect_get_param_by_name(effect, "image");
    if (imageParam)
        gs_effect_set_texture_srgb(imageParam, g_texture);
    gs_draw_sprite(g_texture, 0, kCanvasW, kCanvasH);

    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previousSrgb);
}

static obs_source_info g_sourceInfoInit()
'@

$newText = [regex]::Replace($text, $pattern, $replacement, 1)
if ($newText -eq $text) { throw 'Could not locate sourceVideoRender block.' }
Set-Content $path -Value $newText -Encoding UTF8

if (-not (Select-String -Path $path -Pattern 'gs_effect_set_texture_srgb' -Quiet)) { throw 'Render patch was not applied.' }
if (-not (Select-String -Path $path -Pattern 'gs_draw_sprite' -Quiet)) { throw 'Draw call was not applied.' }
Write-Host 'VyanHQ native OBS compositor render patch applied.'
