using System;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Windows.Media;
using System.Windows.Threading;

internal static class ValidateScreenProtectionMedia
{
    private static int Main(string[] args)
    {
        if (args.Length == 0) return 2;
        string directory = args[0];
        string[] imageNames = { "screen-static-green.png", "screen-static-bars.jpg", "screen-static-warning.bmp" };
        string[] videoNames = { "screen-video-bars.avi", "screen-video-moving.avi", "screen-video-checker.avi" };
        foreach (string name in imageNames)
        {
            string path = Path.Combine(directory, name);
            using (Image image = Image.FromFile(path))
            {
                Console.WriteLine("IMAGE PASS {0} {1}x{2} {3} bytes", name, image.Width, image.Height, new FileInfo(path).Length);
            }
        }
        foreach (string name in videoNames)
        {
            string path = Path.Combine(directory, name);
            if (!ValidateAviHeader(path))
            {
                Console.WriteLine("VIDEO FAIL {0} invalid RIFF/AVI header", name);
                return 1;
            }
            if (!ValidateWithMediaPlayer(path)) return 1;
        }
        return 0;
    }

    private static bool ValidateAviHeader(string path)
    {
        using (FileStream stream = File.OpenRead(path))
        using (BinaryReader reader = new BinaryReader(stream))
        {
            string riff = new string(reader.ReadChars(4));
            int size = reader.ReadInt32();
            string type = new string(reader.ReadChars(4));
            return riff == "RIFF" && type == "AVI " && size == stream.Length - 8;
        }
    }

    private static bool ValidateWithMediaPlayer(string path)
    {
        using (var completed = new ManualResetEventSlim(false))
        {
            var player = new MediaPlayer();
            bool passed = false;
            string error = null;
            int videoWidth = 0;
            int videoHeight = 0;
            player.MediaOpened += delegate
            {
                videoWidth = player.NaturalVideoWidth;
                videoHeight = player.NaturalVideoHeight;
                passed = videoWidth > 0 && videoHeight > 0;
                completed.Set();
            };
            player.MediaFailed += delegate(object sender, ExceptionEventArgs eventArgs)
            {
                error = eventArgs.ErrorException == null ? "unknown media error" : eventArgs.ErrorException.Message;
                completed.Set();
            };
            player.Open(new Uri(Path.GetFullPath(path)));
            DateTime deadline = DateTime.UtcNow.AddSeconds(8);
            while (!completed.IsSet && DateTime.UtcNow < deadline)
            {
                Dispatcher.CurrentDispatcher.Invoke(DispatcherPriority.Background, new Action(delegate { }));
                Thread.Sleep(25);
            }
            player.Close();
            player = null;
            if (!passed)
            {
                Console.WriteLine("VIDEO FAIL {0} {1}", Path.GetFileName(path), error ?? "timeout or no video stream");
                return false;
            }
            Console.WriteLine("VIDEO PASS {0} {1}x{2} {3} bytes", Path.GetFileName(path), videoWidth, videoHeight, new FileInfo(path).Length);
            return true;
        }
    }
}
