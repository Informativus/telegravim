package org.telegram.ui;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import org.telegram.messenger.LocaleController;
import org.telegram.messenger.R;
import org.telegram.messenger.televim.PrivateReadingController;
import org.telegram.messenger.televim.PrivateReadingPolicy;
import org.telegram.ui.ActionBar.ActionBar;
import org.telegram.ui.ActionBar.BaseFragment;
import org.telegram.ui.ActionBar.Theme;
import org.telegram.ui.Cells.TextCheckCell;
import org.telegram.ui.Cells.TextInfoPrivacyCell;
import org.telegram.ui.Cells.TextSettingsCell;
import org.telegram.ui.Components.LayoutHelper;
import org.telegram.ui.Components.RecyclerListView;

/** Native account setting for the local ordinary-chat receipt policy. */
public final class PrivateReadingSettingsActivity extends BaseFragment {
    private static final int ROW_ABOUT = 0;
    private static final int ROW_MASTER = 1;
    private static final int ROW_PRIVATE = 2;
    private static final int ROW_GROUPS = 3;
    private static final int ROW_CHANNELS = 4;
    private static final int ROW_HELP = 5;
    private static final int ROW_STATUS = 6;
    private static final int ROW_RETRY = 7;
    private static final long REFRESH_INTERVAL_MS = 500;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable refresh = this::refreshAndSchedule;
    private PrivateReadingPolicy.Identity identity;
    private PrivateReadingPolicy.Status status;
    private ListAdapter adapter;
    private boolean observing;

    public PrivateReadingSettingsActivity(int account) {
        setCurrentAccount(account);
    }

    @Override
    public boolean onFragmentCreate() {
        status = PrivateReadingController.status(currentAccount);
        if (status == null) {
            return false;
        }
        identity = status.identity;
        return super.onFragmentCreate();
    }

    @Override
    public View createView(Context context) {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back);
        actionBar.setAllowOverlayTitle(true);
        actionBar.setTitle(LocaleController.getString(R.string.TelevimPrivateReading));
        actionBar.setActionBarMenuOnItemClick(new ActionBar.ActionBarMenuOnItemClick() {
            @Override
            public void onItemClick(int id) {
                if (id == -1) {
                    finishFragment();
                }
            }
        });
        FrameLayout frame = new FrameLayout(context);
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray));
        RecyclerListView list = new RecyclerListView(context);
        list.setLayoutManager(new LinearLayoutManager(context, LinearLayoutManager.VERTICAL, false));
        list.setVerticalScrollBarEnabled(false);
        adapter = new ListAdapter(context);
        list.setAdapter(adapter);
        list.setOnItemClickListener((view, position) -> save(position));
        actionBar.setAdaptiveBackground(list);
        frame.addView(list, LayoutHelper.createFrame(
                LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT, Gravity.TOP | Gravity.LEFT));
        fragmentView = frame;
        return fragmentView;
    }

    @Override
    public void onResume() {
        super.onResume();
        observing = true;
        refreshAndSchedule();
    }

    @Override
    public void onPause() {
        observing = false;
        handler.removeCallbacks(refresh);
        super.onPause();
    }

    @Override
    public void onFragmentDestroy() {
        observing = false;
        handler.removeCallbacks(refresh);
        super.onFragmentDestroy();
    }

    private boolean refreshStatus() {
        PrivateReadingPolicy.Status latest = PrivateReadingController.status(currentAccount);
        if (latest == null || latest.identity != identity || !latest.active) {
            observing = false;
            handler.removeCallbacks(refresh);
            finishFragment();
            return false;
        }
        boolean changed = status.loaded != latest.loaded || status.saved != latest.saved
                || status.failure != latest.failure || status.storageFailed != latest.storageFailed
                || status.unsavedProtection != latest.unsavedProtection
                || !status.desired.equals(latest.desired);
        status = latest;
        if (changed && adapter != null) {
            adapter.notifyDataSetChanged();
        }
        return true;
    }

    private void refreshAndSchedule() {
        handler.removeCallbacks(refresh);
        if (observing && refreshStatus()) {
            handler.postDelayed(refresh, REFRESH_INTERVAL_MS);
        }
    }

    private void save(int position) {
        if (!refreshStatus() || !status.loaded) {
            return;
        }
        if ((position < ROW_MASTER || position > ROW_CHANNELS)
                && (position != ROW_RETRY || !canRetry())) {
            return;
        }
        PrivateReadingPolicy.Selection selection = status.desired;
        PrivateReadingController.saveSelection(currentAccount, identity,
                position == ROW_MASTER ? !selection.master : selection.master,
                position == ROW_PRIVATE ? !selection.privateChats : selection.privateChats,
                position == ROW_GROUPS ? !selection.groups : selection.groups,
                position == ROW_CHANNELS ? !selection.channels : selection.channels);
        refreshStatus();
    }

    private boolean canRetry() {
        return status.loaded && !status.saved && status.storageFailed;
    }

    private int statusText() {
        if (!status.loaded) {
            return status.storageFailed
                    ? R.string.TelevimPrivateReadingLoadFailed : R.string.TelevimPrivateReadingLoading;
        }
        if (status.storageFailed) {
            return status.unsavedProtection ? R.string.TelevimPrivateReadingEnableFailed
                    : R.string.TelevimPrivateReadingDisableFailed;
        }
        if (status.failure != PrivateReadingPolicy.Failure.NONE) {
            return R.string.TelevimPrivateReadingUnavailable;
        }
        return status.saved ? R.string.TelevimPrivateReadingSaved : R.string.TelevimPrivateReadingSaving;
    }

    private final class ListAdapter extends RecyclerListView.SelectionAdapter {
        private final Context context;

        ListAdapter(Context context) {
            this.context = context;
        }

        @Override
        public int getItemCount() {
            return canRetry() ? ROW_RETRY + 1 : ROW_STATUS + 1;
        }

        @Override
        public int getItemViewType(int position) {
            return position >= ROW_MASTER && position <= ROW_CHANNELS ? 0 : position == ROW_RETRY ? 1 : 2;
        }

        @Override
        public boolean isEnabled(RecyclerView.ViewHolder holder) {
            int position = holder.getAdapterPosition();
            return status.loaded && ((position >= ROW_MASTER && position <= ROW_CHANNELS)
                    || (position == ROW_RETRY && canRetry()));
        }

        @Override
        public RecyclerView.ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            View view = viewType == 0 ? new TextCheckCell(context)
                    : viewType == 1 ? new TextSettingsCell(context) : new TextInfoPrivacyCell(context);
            if (viewType != 2) {
                view.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite));
            }
            return new RecyclerListView.Holder(view);
        }

        @Override
        public void onBindViewHolder(@NonNull RecyclerView.ViewHolder holder, int position) {
            if (holder.itemView instanceof TextCheckCell) {
                PrivateReadingPolicy.Selection selection = status.desired;
                int label = position == ROW_MASTER ? R.string.TelevimPrivateReadingMaster
                        : position == ROW_PRIVATE ? R.string.TelevimPrivateReadingPrivate
                        : position == ROW_GROUPS ? R.string.TelevimPrivateReadingGroups : R.string.TelevimPrivateReadingChannels;
                boolean checked = position == ROW_MASTER ? selection.master
                        : position == ROW_PRIVATE ? selection.privateChats
                        : position == ROW_GROUPS ? selection.groups : selection.channels;
                TextCheckCell cell = (TextCheckCell) holder.itemView;
                cell.setTextAndCheck(LocaleController.getString(label), checked, position != ROW_CHANNELS);
                cell.setEnabled(status.loaded);
            } else if (position == ROW_RETRY) {
                ((TextSettingsCell) holder.itemView).setText(
                        LocaleController.getString(R.string.TelevimPrivateReadingRetry), false);
            } else {
                TextInfoPrivacyCell cell = (TextInfoPrivacyCell) holder.itemView;
                cell.setFixedSize(0);
                int label = position == ROW_ABOUT ? R.string.TelevimPrivateReadingAbout
                        : position == ROW_HELP ? R.string.TelevimPrivateReadingHelp : statusText();
                cell.setText(LocaleController.getString(label));
            }
        }
    }
}
