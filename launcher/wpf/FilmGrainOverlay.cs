using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Security.Cryptography;
using System.Buffers.Binary;
using System.Text;
using System.Windows.Automation;
using System.Windows.Controls;

namespace DriveClub.Launcher;

// One stationary field at device-pixel resolution, independent of artwork fades.
// No repeating brush, texture scaling, gradients or cached background tiles.
public sealed class FilmGrainOverlay : FrameworkElement
{
    public static readonly DependencyProperty StablePatternProperty = DependencyProperty.Register(
        nameof(StablePattern), typeof(bool), typeof(FilmGrainOverlay),
        new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsRender));
    public bool StablePattern { get => (bool)GetValue(StablePatternProperty); set => SetValue(StablePatternProperty, value); }
    private static readonly Dictionary<(string Key, int Width, int Height, double DpiX, double DpiY), BitmapSource> stableTextures = [];
    private BitmapSource? texture;
    private int pixelWidth, pixelHeight;
    private string? textureKey;

    private string StableKey()
    {
        string? fallbackName = null;
        for (DependencyObject? parent = VisualTreeHelper.GetParent(this); parent != null; parent = VisualTreeHelper.GetParent(parent))
        {
            if (parent is ComboBoxItem item)
            {
                var owner = ItemsControl.ItemsControlFromItemContainer(item);
                return "item:" + (owner == null ? "" : AutomationProperties.GetAutomationId(owner)) + ":" + item.Content;
            }
            if (parent is Control control && AutomationProperties.GetAutomationId(control) is { Length: > 0 } id)
                return "control:" + id;
            if (fallbackName == null && parent is Control { Name.Length: > 0 } named)
                fallbackName = named.Name;
        }
        return "control:" + (fallbackName ?? "unlabelled");
    }

    private static void FillStableNoise(byte[] noise, int width, int height, string key)
    {
        // Hash physical 2-D coordinates, keeping the same pixels on resize and
        // after control recreation. SHA-256 avoids linear PRNG band patterns.
        var input = new byte[40];
        SHA256.HashData(Encoding.UTF8.GetBytes("DriveClub/field-grain/" + key)).CopyTo(input, 0);
        Span<byte> block = stackalloc byte[32];
        for (int y = 0; y < height; ++y)
        {
            BinaryPrimitives.WriteInt32LittleEndian(input.AsSpan(36), y);
            for (int x = 0; x < width; x += 32)
            {
                BinaryPrimitives.WriteInt32LittleEndian(input.AsSpan(32), x / 32);
                SHA256.HashData(input, block);
                block[..Math.Min(32, width - x)].CopyTo(noise.AsSpan(y * width + x));
            }
        }
    }

    public FilmGrainOverlay()
    {
        IsHitTestVisible = false;
        SnapsToDevicePixels = true;
        UseLayoutRounding = true;
        RenderOptions.SetBitmapScalingMode(this, BitmapScalingMode.NearestNeighbor);
    }

    protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
    {
        base.OnRenderSizeChanged(sizeInfo); InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawing)
    {
        base.OnRender(drawing);
        var dpi = VisualTreeHelper.GetDpi(this);
        int width = (int)Math.Round(ActualWidth * dpi.DpiScaleX);
        int height = (int)Math.Round(ActualHeight * dpi.DpiScaleY);
        if (width <= 0 || height <= 0) return;
        string? key = StablePattern ? StableKey() : null;
        if (texture == null || pixelWidth != width || pixelHeight != height || textureKey != key)
        {
            var cacheKey = (key ?? "", width, height, dpi.PixelsPerInchX, dpi.PixelsPerInchY);
            if (key != null && stableTextures.TryGetValue(cacheKey, out var cached)) texture = cached;
            else
            {
                var pixels = new byte[checked(width * height * 4)];
                var noise = new byte[checked(width * height)];
                if (key == null) RandomNumberGenerator.Fill(noise);
                else FillStableNoise(noise, width, height, key);
                for (int i = 0; i < pixels.Length; i += 4)
                {
                    byte tone = (noise[i / 4] & 1) == 0 ? (byte)0 : (byte)255;
                    pixels[i] = pixels[i + 1] = pixels[i + 2] = tone;
                    pixels[i + 3] = 5; // Constant strength; no spatial alpha ramp.
                }
                texture = BitmapSource.Create(width, height, dpi.PixelsPerInchX, dpi.PixelsPerInchY,
                    PixelFormats.Bgra32, null, pixels, width * 4);
                texture.Freeze();
                if (key != null)
                {
                    if (stableTextures.Count >= 128) stableTextures.Clear();
                    stableTextures[cacheKey] = texture;
                }
            }
            pixelWidth = width; pixelHeight = height; textureKey = key;
        }
        // Ceil plus stretching to a fractional DIP extent can add one texel and
        // create periodic resampling bands at non-100% Windows display scales.
        double renderWidth = width / dpi.DpiScaleX, renderHeight = height / dpi.DpiScaleY;
        drawing.PushGuidelineSet(new GuidelineSet([0, renderWidth], [0, renderHeight]));
        drawing.DrawImage(texture, new Rect(0, 0, renderWidth, renderHeight));
        drawing.Pop();
    }
}
