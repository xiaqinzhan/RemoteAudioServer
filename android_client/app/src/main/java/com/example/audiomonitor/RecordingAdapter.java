package com.example.audiomonitor;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import java.util.List;

public class RecordingAdapter extends RecyclerView.Adapter<RecordingAdapter.VH> {

    public interface OnPlay { void onPlay(Recording r, int position); }

    private List<Recording> items;
    private int playingPos = -1;
    private final OnPlay onPlay;

    public RecordingAdapter(List<Recording> items, OnPlay onPlay) {
        this.items = items;
        this.onPlay = onPlay;
    }

    public void setItems(List<Recording> items) {
        this.items = items;
        this.playingPos = -1;
        notifyDataSetChanged();
    }

    public void setPlaying(int pos) {
        this.playingPos = pos;
        notifyDataSetChanged();
    }

    @NonNull
    @Override
    public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View v = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_recording, parent, false);
        return new VH(v);
    }

    @Override
    public void onBindViewHolder(@NonNull VH h, int position) {
        Recording r = items.get(position);
        h.name.setText(r.name);
        h.info.setText(String.format("时长 %ds · %.1f KB", r.duration, r.size / 1024.0));
        if (position == playingPos) {
            h.state.setText("播放中… 点击停止");
            h.state.setTextColor(0xFFEF4444);
        } else {
            h.state.setText("播放");
            h.state.setTextColor(0xFF22D3EE);
        }
        h.itemView.setOnClickListener(v -> {
            if (onPlay != null) onPlay.onPlay(r, position);
        });
    }

    @Override
    public int getItemCount() { return items == null ? 0 : items.size(); }

    static class VH extends RecyclerView.ViewHolder {
        final TextView name, info, state;
        VH(@NonNull View itemView) {
            super(itemView);
            name = itemView.findViewById(R.id.tvName);
            info = itemView.findViewById(R.id.tvInfo);
            state = itemView.findViewById(R.id.tvPlayState);
        }
    }
}
