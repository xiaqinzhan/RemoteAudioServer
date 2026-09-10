package com.example.audiomonitor;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import java.util.List;

public class DeviceAdapter extends RecyclerView.Adapter<DeviceAdapter.VH> {

    public interface OnPick { void onPick(DeviceInfo d); }

    private List<DeviceInfo> items;
    private String selectedId;
    private final OnPick onPick;

    public DeviceAdapter(List<DeviceInfo> items, OnPick onPick) {
        this.items = items;
        this.onPick = onPick;
    }

    public void setItems(List<DeviceInfo> items) {
        this.items = items;
        notifyDataSetChanged();
    }

    public void setSelected(String id) {
        this.selectedId = id;
        notifyDataSetChanged();
    }

    @NonNull
    @Override
    public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View v = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_device, parent, false);
        return new VH(v);
    }

    @Override
    public void onBindViewHolder(@NonNull VH h, int position) {
        DeviceInfo d = items.get(position);
        h.id.setText(d.deviceId);
        String rec = d.recordingEnabled ? "录音开" : "录音关";
        String sd = d.sdOk ? "SD正常" : "SD异常";
        h.meta.setText(String.format("固件 %s · 监听 %d · %s · %s",
                d.fw.isEmpty() ? "-" : d.fw, d.listenerCount, rec, sd));
        h.dot.setBackgroundResource(d.online ? R.drawable.dot_online : R.drawable.dot_offline);
        h.selected.setVisibility(
                (d.deviceId.equals(selectedId)) ? View.VISIBLE : View.GONE);

        h.itemView.setAlpha(d.online ? 1f : 0.55f);
        h.itemView.setOnClickListener(v -> {
            if (onPick != null) onPick.onPick(d);
        });
    }

    @Override
    public int getItemCount() { return items == null ? 0 : items.size(); }

    static class VH extends RecyclerView.ViewHolder {
        final View dot;
        final TextView id, meta, selected;
        VH(@NonNull View itemView) {
            super(itemView);
            dot = itemView.findViewById(R.id.dot);
            id = itemView.findViewById(R.id.tvId);
            meta = itemView.findViewById(R.id.tvMeta);
            selected = itemView.findViewById(R.id.tvSelected);
        }
    }
}
