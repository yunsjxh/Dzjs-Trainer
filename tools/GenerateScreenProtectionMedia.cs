using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;

internal static class GenerateScreenProtectionMedia
{
    private const int Width = 640;
    private const int Height = 360;
    private const int Fps = 12;
    private const int Frames = 36;

    private static void Main(string[] args)
    {
        string output = args.Length == 0 ? Path.Combine(Environment.CurrentDirectory, "test-media", "screen-protection-20260906") : args[0];
        Directory.CreateDirectory(output);

        SaveImage(Path.Combine(output, "screen-static-green.png"), ImageFormat.Png, DrawGreen);
        SaveImage(Path.Combine(output, "screen-static-bars.jpg"), ImageFormat.Jpeg, DrawBars);
        SaveImage(Path.Combine(output, "screen-static-warning.bmp"), ImageFormat.Bmp, DrawWarning);
        WriteAvi(Path.Combine(output, "screen-video-bars.avi"), DrawVideoBars);
        WriteAvi(Path.Combine(output, "screen-video-moving.avi"), DrawVideoMoving);
        WriteAvi(Path.Combine(output, "screen-video-checker.avi"), DrawVideoChecker);

        File.WriteAllText(Path.Combine(output, "README.txt"),
            "Screen protection test media generated 2026-09-06\r\n" +
            "Images: PNG, JPG, BMP. Videos: uncompressed 24-bit AVI, 640x360, 12 fps, 3 seconds.\r\n" +
            "Use each file in the screen protection media picker and verify that the preview and replacement surface update.\r\n");
    }

    private static void SaveImage(string path, ImageFormat format, Action<Graphics> draw)
    {
        using (var bitmap = new Bitmap(Width, Height, PixelFormat.Format24bppRgb))
        using (var graphics = Graphics.FromImage(bitmap))
        {
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            draw(graphics);
            bitmap.Save(path, format);
        }
    }

    private static void DrawGreen(Graphics g)
    {
        g.Clear(Color.FromArgb(18, 116, 74));
        using (var pen = new Pen(Color.FromArgb(80, 230, 160), 4))
        using (var brush = new SolidBrush(Color.FromArgb(25, 180, 105)))
        using (var font = new Font("Arial", 30, FontStyle.Bold))
        {
            g.FillRectangle(brush, 24, 24, Width - 48, Height - 48);
            g.DrawRectangle(pen, 24, 24, Width - 48, Height - 48);
            g.DrawString("SCREEN PROTECTION / IMAGE", font, Brushes.White, 72, 142);
        }
    }

    private static void DrawBars(Graphics g)
    {
        Color[] colors = { Color.White, Color.Yellow, Color.Cyan, Color.Lime, Color.Magenta, Color.Red, Color.Blue, Color.Black };
        int barWidth = Width / colors.Length;
        g.Clear(Color.Black);
        for (int i = 0; i < colors.Length; i++)
            using (var brush = new SolidBrush(colors[i]))
                g.FillRectangle(brush, i * barWidth, 0, barWidth + 1, 250);
        using (var brush = new SolidBrush(Color.FromArgb(32, 32, 32)))
            g.FillRectangle(brush, 0, 250, Width, 110);
        using (var font = new Font("Arial", 28, FontStyle.Bold))
            g.DrawString("STATIC COLOR BARS / JPG", font, Brushes.White, 112, 288);
    }

    private static void DrawWarning(Graphics g)
    {
        g.Clear(Color.FromArgb(34, 34, 34));
        using (var brush = new SolidBrush(Color.FromArgb(214, 44, 44)))
        using (var pen = new Pen(Color.FromArgb(255, 225, 85), 8))
        using (var font = new Font("Arial", 38, FontStyle.Bold))
        using (var smallFont = new Font("Arial", 23, FontStyle.Bold))
        {
            g.FillRectangle(brush, 42, 40, Width - 84, Height - 80);
            g.DrawRectangle(pen, 42, 40, Width - 84, Height - 80);
            g.DrawString("PROTECTED", font, Brushes.White, 178, 112);
            g.DrawString("STATIC WARNING / BMP", smallFont, Brushes.White, 155, 210);
        }
    }

    private static void WriteAvi(string path, Action<Bitmap, int> drawFrame)
    {
        int rowBytes = (Width * 3 + 3) & ~3;
        int frameBytes = rowBytes * Height;
        using (var stream = File.Create(path))
        using (var writer = new BinaryWriter(stream))
        {
            WriteFourCC(writer, "RIFF");
            WriteInt32(writer, 0);
            WriteFourCC(writer, "AVI ");

            long hdrlStart = BeginList(writer, "hdrl");
            long avihStart = BeginChunk(writer, "avih");
            WriteInt32(writer, 1000000 / Fps);
            WriteInt32(writer, Width * Height * 3 * Fps);
            WriteInt32(writer, 0);
            WriteInt32(writer, 0x10);
            WriteInt32(writer, Frames);
            WriteInt32(writer, frameBytes);
            WriteInt32(writer, 1);
            WriteInt32(writer, 0);
            WriteInt32(writer, Width);
            WriteInt32(writer, Height);
            EndChunk(writer, avihStart);

            long strlStart = BeginList(writer, "strl");
            long strhStart = BeginChunk(writer, "strh");
            WriteFourCC(writer, "vids");
            WriteFourCC(writer, "DIB ");
            WriteInt32(writer, 0);
            WriteInt16(writer, 0);
            WriteInt16(writer, 0);
            WriteInt32(writer, 0);
            WriteInt32(writer, 1);
            WriteInt32(writer, Fps);
            WriteInt32(writer, 0);
            WriteInt32(writer, Frames);
            WriteInt32(writer, frameBytes);
            WriteInt32(writer, -1);
            WriteInt32(writer, frameBytes);
            WriteInt16(writer, 0);
            WriteInt16(writer, 0);
            WriteInt16(writer, (short)Width);
            WriteInt16(writer, (short)Height);
            EndChunk(writer, strhStart);

            long strfStart = BeginChunk(writer, "strf");
            WriteInt32(writer, 40);
            WriteInt32(writer, Width);
            WriteInt32(writer, Height);
            WriteInt16(writer, 1);
            WriteInt16(writer, 24);
            WriteInt32(writer, 0);
            WriteInt32(writer, frameBytes);
            WriteInt32(writer, 0);
            WriteInt32(writer, 0);
            WriteInt32(writer, 0);
            WriteInt32(writer, 0);
            EndChunk(writer, strfStart);
            EndList(writer, strlStart);
            EndList(writer, hdrlStart);

            long moviStart = BeginList(writer, "movi");
            long[] frameOffsets = new long[Frames];
            for (int frame = 0; frame < Frames; frame++)
            {
                using (var bitmap = new Bitmap(Width, Height, PixelFormat.Format24bppRgb))
                using (var graphics = Graphics.FromImage(bitmap))
                {
                    graphics.SmoothingMode = SmoothingMode.None;
                    drawFrame(bitmap, frame);
                    frameOffsets[frame] = stream.Position - moviStart - 4;
                    WriteFourCC(writer, "00db");
                    WriteInt32(writer, frameBytes);
                    WriteBitmapPixels(writer, bitmap, rowBytes);
                }
            }
            EndList(writer, moviStart);

            long idxStart = BeginChunk(writer, "idx1");
            for (int frame = 0; frame < Frames; frame++)
            {
                WriteFourCC(writer, "00db");
                WriteInt32(writer, 0x10);
                WriteInt32(writer, (int)frameOffsets[frame]);
                WriteInt32(writer, frameBytes);
            }
            EndChunk(writer, idxStart);

            long fileEnd = stream.Position;
            stream.Position = 4;
            WriteInt32(writer, checked((int)fileEnd - 8));
            stream.Position = fileEnd;
        }
    }

    private static void WriteBitmapPixels(BinaryWriter writer, Bitmap bitmap, int rowBytes)
    {
        var rectangle = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
        BitmapData data = bitmap.LockBits(rectangle, ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        try
        {
            byte[] row = new byte[rowBytes];
            for (int y = bitmap.Height - 1; y >= 0; y--)
            {
                IntPtr source = IntPtr.Add(data.Scan0, y * data.Stride);
                System.Runtime.InteropServices.Marshal.Copy(source, row, 0, rowBytes);
                writer.Write(row);
            }
        }
        finally
        {
            bitmap.UnlockBits(data);
        }
    }

    private static void DrawVideoBars(Bitmap bitmap, int frame)
    {
        using (var g = Graphics.FromImage(bitmap))
        {
            DrawBars(g);
            int x = (frame * 20) % Width;
            using (var pen = new Pen(Color.White, 7)) g.DrawLine(pen, x, 0, x, Height);
            using (var font = new Font("Arial", 18, FontStyle.Bold)) g.DrawString("VIDEO BARS", font, Brushes.White, 18, 18);
        }
    }

    private static void DrawVideoMoving(Bitmap bitmap, int frame)
    {
        using (var g = Graphics.FromImage(bitmap))
        {
            g.Clear(Color.FromArgb(14, 26, 54));
            int offset = (frame * 18) % 96;
            for (int x = -96 + offset; x < Width + 96; x += 96)
                using (var brush = new SolidBrush(Color.FromArgb(32, 180, 220)))
                    g.FillRectangle(brush, x, 0, 46, Height);
            using (var pen = new Pen(Color.FromArgb(250, 220, 80), 6)) g.DrawRectangle(pen, 10, 10, Width - 20, Height - 20);
            using (var font = new Font("Arial", 28, FontStyle.Bold)) g.DrawString("MOVING STRIPES", font, Brushes.White, 166, 150);
        }
    }

    private static void DrawVideoChecker(Bitmap bitmap, int frame)
    {
        using (var g = Graphics.FromImage(bitmap))
        {
            g.Clear(Color.Black);
            int size = 45;
            int shift = frame % 2;
            for (int y = 0; y < Height; y += size)
                for (int x = 0; x < Width; x += size)
                    if (((x / size) + (y / size) + shift) % 2 == 0)
                        using (var brush = new SolidBrush(Color.FromArgb(244, 86, 86))) g.FillRectangle(brush, x, y, size, size);
            using (var brush = new SolidBrush(Color.FromArgb(30, 30, 30))) g.FillRectangle(brush, 145, 132, 350, 96);
            using (var font = new Font("Arial", 28, FontStyle.Bold)) g.DrawString("CHECKER FLIP", font, Brushes.White, 198, 158);
        }
    }

    private static long BeginList(BinaryWriter writer, string type)
    {
        WriteFourCC(writer, "LIST");
        long sizePosition = writer.BaseStream.Position;
        WriteInt32(writer, 0);
        WriteFourCC(writer, type);
        return sizePosition;
    }

    private static void EndList(BinaryWriter writer, long sizePosition)
    {
        long end = writer.BaseStream.Position;
        writer.BaseStream.Position = sizePosition;
        WriteInt32(writer, checked((int)(end - sizePosition - 4)));
        writer.BaseStream.Position = end;
        if ((end - sizePosition) % 2 != 0) writer.Write((byte)0);
    }

    private static long BeginChunk(BinaryWriter writer, string type)
    {
        WriteFourCC(writer, type);
        long sizePosition = writer.BaseStream.Position;
        WriteInt32(writer, 0);
        return sizePosition;
    }

    private static void EndChunk(BinaryWriter writer, long sizePosition)
    {
        long end = writer.BaseStream.Position;
        writer.BaseStream.Position = sizePosition;
        WriteInt32(writer, checked((int)(end - sizePosition - 4)));
        writer.BaseStream.Position = end;
        if ((end - sizePosition) % 2 != 0) writer.Write((byte)0);
    }

    private static void WriteFourCC(BinaryWriter writer, string value)
    {
        writer.Write(new[] { value[0], value[1], value[2], value[3] }.Select(c => (byte)c).ToArray());
    }

    private static void WriteInt16(BinaryWriter writer, short value) { writer.Write(value); }
    private static void WriteInt32(BinaryWriter writer, int value) { writer.Write(value); }
}
