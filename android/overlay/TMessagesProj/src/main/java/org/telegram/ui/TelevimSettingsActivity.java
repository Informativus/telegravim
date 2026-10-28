package org.telegram.ui;

import android.content.Context;
import android.content.Intent;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import org.telegram.messenger.LocaleController;
import org.telegram.messenger.R;
import org.telegram.ui.ActionBar.ActionBar;
import org.telegram.ui.ActionBar.BaseFragment;
import org.telegram.ui.ActionBar.Theme;
import org.telegram.ui.Cells.HeaderCell;
import org.telegram.ui.Cells.TextInfoPrivacyCell;
import org.telegram.ui.Cells.TextSettingsCell;
import org.telegram.ui.Components.LayoutHelper;
import org.telegram.ui.Components.RecyclerListView;

public final class TelevimSettingsActivity extends BaseFragment {
    private static final int ROW_HEADER = 0;
    private static final int ROW_IMPORT = 1;
    private static final int ROW_PRIVATE_READING = 2;
    private static final int ROW_ABOUT = 3;
    private static final int ROW_COUNT = 4;

    @Override
    public View createView(Context context) {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back);
        actionBar.setAllowOverlayTitle(true);
        actionBar.setTitle(LocaleController.getString(R.string.TelevimSettingsTitle));
        actionBar.setActionBarMenuOnItemClick(new ActionBar.ActionBarMenuOnItemClick() {
            @Override
            public void onItemClick(int id) {
                if (id == -1) {
                    finishFragment();
                }
            }
        });

        final FrameLayout frame = new FrameLayout(context);
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray));

        final RecyclerListView list = new RecyclerListView(context);
        list.setLayoutManager(new LinearLayoutManager(context, LinearLayoutManager.VERTICAL, false));
        list.setVerticalScrollBarEnabled(false);
        list.setAdapter(new ListAdapter(context));
        list.setOnItemClickListener((view, position) -> {
            if (position == ROW_IMPORT) {
                getParentActivity().startActivity(new Intent(getParentActivity(), OfflineSettingsActivity.class));
            } else if (position == ROW_PRIVATE_READING) {
                getParentActivity().startActivity(new Intent(getParentActivity(), PrivateReadingSettingsActivity.class)
                        .putExtra("account", currentAccount));
            }
        });
        actionBar.setAdaptiveBackground(list);
        frame.addView(list, LayoutHelper.createFrame(
                LayoutHelper.MATCH_PARENT,
                LayoutHelper.MATCH_PARENT,
                Gravity.TOP | Gravity.LEFT));
        fragmentView = frame;
        return fragmentView;
    }

    private static final class ListAdapter extends RecyclerListView.SelectionAdapter {
        private final Context context;

        private ListAdapter(Context context) {
            this.context = context;
        }

        @Override
        public int getItemCount() {
            return ROW_COUNT;
        }

        @Override
        public int getItemViewType(int position) {
            return position;
        }

        @Override
        public RecyclerView.ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            final View view;
            if (viewType == ROW_HEADER) {
                view = new HeaderCell(context);
            } else if (viewType == ROW_ABOUT) {
                view = new TextInfoPrivacyCell(context);
            } else {
                view = new TextSettingsCell(context);
                view.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite));
            }
            return new RecyclerListView.Holder(view);
        }

        @Override
        public void onBindViewHolder(@NonNull RecyclerView.ViewHolder holder, int position) {
            if (position == ROW_HEADER) {
                ((HeaderCell) holder.itemView).setText(LocaleController.getString(R.string.TelevimSettingsTitle));
            } else if (position == ROW_IMPORT) {
                ((TextSettingsCell) holder.itemView).setText(
                        LocaleController.getString(R.string.TelevimImportTitle), true);
            } else if (position == ROW_PRIVATE_READING) {
                ((TextSettingsCell) holder.itemView).setText(
                        LocaleController.getString(R.string.TelevimPrivateReading), false);
            } else if (position == ROW_ABOUT) {
                final TextInfoPrivacyCell cell = (TextInfoPrivacyCell) holder.itemView;
                cell.setFixedSize(0);
                cell.setText(LocaleController.getString(R.string.TelevimSettingsAbout));
            }
        }

        @Override
        public boolean isEnabled(RecyclerView.ViewHolder holder) {
            final int position = holder.getAdapterPosition();
            return position == ROW_IMPORT || position == ROW_PRIVATE_READING;
        }
    }
}
