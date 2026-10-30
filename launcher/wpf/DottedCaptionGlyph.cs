using System.Windows;
using System.Windows.Media;

namespace DriveClub.Launcher;

// Vector circles keep the caption glyphs round at every monitor DPI.
public sealed class DottedCaptionGlyph : FrameworkElement
{
    public static readonly DependencyProperty GlyphProperty = DependencyProperty.Register(
        nameof(Glyph), typeof(string), typeof(DottedCaptionGlyph),
        new FrameworkPropertyMetadata("Close", FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty FillProperty = DependencyProperty.Register(
        nameof(Fill), typeof(Brush), typeof(DottedCaptionGlyph),
        new FrameworkPropertyMetadata(Brushes.LightGray, FrameworkPropertyMetadataOptions.AffectsRender));

    public string Glyph { get => (string)GetValue(GlyphProperty); set => SetValue(GlyphProperty, value); }
    public Brush Fill { get => (Brush)GetValue(FillProperty); set => SetValue(FillProperty, value); }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        if (Glyph == "ExternalLink")
        {
            // Eight dots match the reference: three across the top, three
            // down the right, and an uninterrupted diagonal to the corner.
            // Keep the 16-DIP caption's pitch and diameter without scaling.
            const double pitch = 16d / 5;
            double externalDotRadius = pitch * .29;
            Point[] dots = [new(1, 0), new(2, 0), new(3, 0), new(2, 1),
                new(3, 1), new(1, 2), new(3, 2), new(0, 3)];
            foreach (var p in dots)
                dc.DrawEllipse(Fill, null, new Point(ActualWidth / 2 + (p.X - 1.5) * pitch, ActualHeight / 2 + (p.Y - 1.5) * pitch), externalDotRadius, externalDotRadius);
            return;
        }
        if (Glyph == "Search")
        {
            // Match the 16-DIP, five-column caption/arrow glyphs exactly.
            const double spacing = 16d / 5;
            double dotRadius = spacing * .29;
            double lensRadius = spacing / (2 * Math.Sin(Math.PI / 12));
            double tailEnd = (lensRadius + 2 * spacing) / Math.Sqrt(2);
            double centerOffset = (tailEnd - lensRadius) / 2;
            var lensCenter = new Point(ActualWidth / 2 - centerOffset, ActualHeight / 2 - centerOffset);
            for (int i = 0; i < 12; ++i)
            {
                double angle = i * Math.PI / 6;
                dc.DrawEllipse(Fill, null, new Point(lensCenter.X + lensRadius * Math.Cos(angle), lensCenter.Y + lensRadius * Math.Sin(angle)), dotRadius, dotRadius);
            }
            for (int dot = 1; dot <= 2; ++dot)
            {
                double offset = (lensRadius + dot * spacing) / Math.Sqrt(2);
                dc.DrawEllipse(Fill, null, new Point(lensCenter.X + offset, lensCenter.Y + offset), dotRadius, dotRadius);
            }
            return;
        }
        if (Glyph == "TriangleDown")
        {
            double extent = Math.Min(ActualWidth, ActualHeight);
            double side = extent * .8, height = side * Math.Sqrt(3) / 2;
            double x = (ActualWidth - side) / 2, y = (ActualHeight - height) / 2;
            Point[] vertices = [new(x, y), new(x + side, y), new(x + side / 2, y + height)];
            double dotRadius = extent / 5 * .29;
            for (int edge = 0; edge < 3; ++edge)
                for (int dot = 0; dot < 4; ++dot)
                {
                    Point a = vertices[edge], b = vertices[(edge + 1) % 3];
                    double t = dot / 4.0;
                    dc.DrawEllipse(Fill, null, new Point(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t), dotRadius, dotRadius);
                }
            return;
        }
        string[] rows = Glyph switch
        {
            "Checkmark" => ["....#", "...#.", "#.#..", ".#...", "....."],
            "ChevronDown" => [".....", "#...#", ".#.#.", "..#..", "....."],
            "Minimize" => [".....", ".....", "#####", ".....", "....."],
            "Maximize" => ["#####", "#...#", "#...#", "#...#", "#####"],
            "Restore" => ["..#####", "..#...#", "#####.#", "#...#.#", "#...###", "#...#..", "#####.."],
            _ => ["#...#", ".#.#.", "..#..", ".#.#.", "#...#"]
        };
        int count = rows.Length;
        double size = Math.Min(ActualWidth, ActualHeight), step = size / count;
        double radius = step * .29;
        double left = (ActualWidth - size) / 2, top = (ActualHeight - size) / 2;
        for (int y = 0; y < count; ++y)
            for (int x = 0; x < count; ++x)
                if (rows[y][x] == '#')
                    dc.DrawEllipse(Fill, null, new Point(left + (x + .5) * step, top + (y + .5) * step), radius, radius);
    }
}
