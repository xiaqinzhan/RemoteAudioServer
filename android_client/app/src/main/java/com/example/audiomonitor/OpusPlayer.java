package com.example.audiomonitor;

import android.media.MediaPlayer;

import java.io.File;
import java.io.FileOutputStream;

/**
 * 回放历史录音：把收到的 Ogg Opus 文件字节写入临时文件，用 MediaPlayer 播放。
 */
public class OpusPlayer {

    public interface StateListener {
        void onPlaybackStarted(long durationMs);
        void onCompletion();
        void onError(String msg);
    }

    private MediaPlayer player;
    private File tempFile;
    private StateListener listener;

    public void setListener(StateListener l) { this.listener = l; }

    public void play(byte[] opusData, File cacheDir) {
        stop();
        try {
            tempFile = new File(cacheDir, "recording_" + System.currentTimeMillis() + ".opus");
            try (FileOutputStream fos = new FileOutputStream(tempFile)) {
                fos.write(opusData);
            }
            player = new MediaPlayer();
            player.setDataSource(tempFile.getAbsolutePath());
            player.setOnPreparedListener(mp -> {
                mp.start();
                if (listener != null) listener.onPlaybackStarted(mp.getDuration());
            });
            player.setOnCompletionListener(mp -> {
                if (listener != null) listener.onCompletion();
            });
            player.setOnErrorListener((mp, what, extra) -> {
                if (listener != null) listener.onError("播放失败(what=" + what + ")，此设备可能不支持 Ogg/Opus");
                return true;
            });
            player.prepareAsync();
        } catch (Exception e) {
            if (listener != null) listener.onError("无法播放: " + e.getMessage());
        }
    }

    public void stop() {
        if (player != null) {
            try {
                player.reset();
                player.release();
            } catch (Exception ignored) {}
            player = null;
        }
        if (tempFile != null) {
            // noinspection ResultOfMethodCallIgnored
            tempFile.delete();
            tempFile = null;
        }
    }
}
