using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace CeBridgeClient;

internal static class Program
{
    private const string PipeName = "codex_ce_bridge_v1";
    private const int DefaultTimeoutMilliseconds = 10_000;
    private const int MaximumResponseSize = 16 * 1024 * 1024;

    public static async Task<int> Main(string[] args)
    {
        try
        {
            var request = ParseRequest(args);
            var response = await ExecuteAsync(request.Source, request.TimeoutMilliseconds);
            using var document = JsonDocument.Parse(response);
            Console.WriteLine(JsonSerializer.Serialize(document.RootElement, new JsonSerializerOptions
            {
                WriteIndented = true,
            }));

            return document.RootElement.TryGetProperty("ok", out var ok) && ok.GetBoolean() ? 0 : 1;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(JsonSerializer.Serialize(new
            {
                ok = false,
                error = exception.Message,
                type = exception.GetType().Name,
            }, new JsonSerializerOptions { WriteIndented = true }));
            return 2;
        }
    }

    private static Request ParseRequest(string[] args)
    {
        var timeout = DefaultTimeoutMilliseconds;
        var index = 0;
        if (args.Length >= 2 && args[0] == "--timeout")
        {
            if (!int.TryParse(args[1], out timeout) || timeout <= 0)
            {
                throw new ArgumentException("--timeout must be a positive number of milliseconds.");
            }

            index = 2;
        }

        if (index >= args.Length || args[index] is "-h" or "--help" or "help")
        {
            throw new ArgumentException(
                "Usage: CeBridgeClient [--timeout <ms>] ping | exec <lua> | file <path>");
        }

        var source = args[index] switch
        {
            "ping" => "return 'pong', getOpenedProcessID(), getCEVersion()",
            "exec" when args.Length > index + 1 => string.Join(' ', args.Skip(index + 1)),
            "file" when args.Length == index + 2 => File.ReadAllText(args[index + 1], Encoding.UTF8),
            _ => throw new ArgumentException(
                "Usage: CeBridgeClient [--timeout <ms>] ping | exec <lua> | file <path>"),
        };

        return new Request(source, timeout);
    }

    private static async Task<string> ExecuteAsync(string source, int timeoutMilliseconds)
    {
        var request = Encoding.UTF8.GetBytes(source);
        await using var pipe = new NamedPipeClientStream(
            ".",
            PipeName,
            PipeDirection.InOut,
            PipeOptions.Asynchronous);

        using var timeout = new CancellationTokenSource(timeoutMilliseconds);
        await pipe.ConnectAsync(timeout.Token);

        var requestLength = BitConverter.GetBytes((uint)request.Length);
        await pipe.WriteAsync(requestLength, timeout.Token);
        await pipe.WriteAsync(request, timeout.Token);
        await pipe.FlushAsync(timeout.Token);

        var responseLengthBytes = new byte[sizeof(uint)];
        await ReadExactlyAsync(pipe, responseLengthBytes, timeout.Token);
        var responseLength = BitConverter.ToUInt32(responseLengthBytes);
        if (responseLength > MaximumResponseSize)
        {
            throw new InvalidDataException($"Bridge response is too large: {responseLength} bytes.");
        }

        var response = new byte[responseLength];
        await ReadExactlyAsync(pipe, response, timeout.Token);
        return Encoding.UTF8.GetString(response);
    }

    private static async Task ReadExactlyAsync(
        Stream stream,
        Memory<byte> destination,
        CancellationToken cancellationToken)
    {
        var offset = 0;
        while (offset < destination.Length)
        {
            var read = await stream.ReadAsync(destination[offset..], cancellationToken);
            if (read == 0)
            {
                throw new EndOfStreamException("The CE bridge closed the pipe unexpectedly.");
            }

            offset += read;
        }
    }

    private sealed record Request(string Source, int TimeoutMilliseconds);
}
