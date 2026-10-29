package org.telegram.messenger.televim;

import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.TreeSet;
import java.util.UUID;

public final class PrivateReadingPolicy {
    public static final int MAX_CONVERSATIONS = 1024;
    public static final int MAX_CONTENT_IDS = 8192;
    public static final int MAX_SENDS = 512;
    public static final int MAX_RELEASES = 64;
    public static final int MAX_REQUESTS = 16;
    public static final int MAX_REQUEST_IDS = 100;
    public static final int MAX_NOTIFICATIONS = 256;
    public static final int MAX_RETAINED_BYTES = 0x200000;
    private static final int RELEASE_RETAINED_BYTES = 2048;
    private final Identity identity;
    private boolean active = true;
    private boolean loaded;
    private Selection published = Selection.off();
    private Selection durable = Selection.off();
    private Selection desired = Selection.off();
    private long transition;
    private long savedTransition = -1L;
    private int fenceMask;
    private Failure failure = Failure.NONE;
    private boolean storageFailed;
    private long masterGeneration;
    private long privateGeneration;
    private long groupGeneration;
    private long channelGeneration;
    private long botGeneration;
    private long sequence;
    private int contentCount;
    private int requestContentCount;
    private int transportPayloadBytes;
    private int committing;
    private final Map<Conversation, Pending> pending = new HashMap<Conversation, Pending>();
    private final Map<Conversation, Classification> keys = new HashMap<Conversation, Classification>();
    private final Map<Long, SendAuthorization> sends = new HashMap<Long, SendAuthorization>();
    private final Map<Long, Release> releases = new LinkedHashMap<Long, Release>();
    private final Map<RequestToken, Boolean> requests = new LinkedHashMap<RequestToken, Boolean>();
    private final Map<String, NotificationAction> notifications = new HashMap<String, NotificationAction>();
    private SpecialConsent special;

    public PrivateReadingPolicy(Environment environment, long l) {
        this.identity = new Identity(environment, l);
    }

    public Identity identity() {
        return this.identity;
    }

    public synchronized boolean owns(Identity identity) {
        return this.active && identity == this.identity;
    }

    public synchronized Status status() {
        return new Status(this);
    }

    public synchronized ReceiptDecision decision(Identity identity, Conversation conversation) {
        if (!this.owns(identity)) {
            return ReceiptDecision.INACTIVE;
        }
        Objects.requireNonNull(conversation);
        Classification classification = this.keys.get(conversation);
        if (classification != null && classification.category != conversation.category) {
            classification.category = conversation.category;
            classification.revision = ++this.sequence;
            this.invalidateStale();
        }
        if (conversation.mask() == 0) {
            return ReceiptDecision.NATIVE;
        }
        if (!this.loaded) {
            return ReceiptDecision.NOT_LOADED;
        }
        return ((this.published.mask() | this.durable.mask() | this.fenceMask) & conversation.mask()) != 0 ? ReceiptDecision.WITHHOLD : ReceiptDecision.NATIVE;
    }

    public synchronized ReceiptDecision decisionForUnknownConversation(Identity identity) {
        if (!this.owns(identity)) {
            return ReceiptDecision.INACTIVE;
        }
        if (!this.loaded) {
            return ReceiptDecision.NOT_LOADED;
        }
        return (this.published.mask() | this.durable.mask() | this.fenceMask) != 0
                ? ReceiptDecision.WITHHOLD : ReceiptDecision.NATIVE;
    }

    synchronized boolean loaded(Identity identity, Selection selection) {
        if (!this.owns(identity) || this.loaded || this.transition != 0L) {
            return false;
        }
        this.durable = this.desired = Objects.requireNonNull(selection);
        this.published = this.desired;
        this.loaded = true;
        this.savedTransition = 0L;
        this.storageFailed = false;
        this.failure = Failure.NONE;
        return true;
    }

    synchronized void loadFailed(Identity identity) {
        if (this.owns(identity) && !this.loaded) {
            this.storageFailed = true;
            this.failure = Failure.STORAGE;
        }
    }

    synchronized long beginSave(Identity identity, Selection selection) {
        if (!this.owns(identity)) {
            return -1L;
        }
        Objects.requireNonNull(selection);
        Selection selection2 = this.desired;
        this.desired = selection;
        ++this.transition;
        this.fenceMask |= this.published.mask() | this.durable.mask() | selection.mask();
        if (selection2.master != selection.master) {
            ++this.masterGeneration;
        }
        if (selection2.privateChats != selection.privateChats) {
            ++this.privateGeneration;
        }
        if (selection2.groups != selection.groups) {
            ++this.groupGeneration;
        }
        if (selection2.channels != selection.channels) {
            ++this.channelGeneration;
        }
        if (selection2.bots != selection.bots) {
            ++this.botGeneration;
        }
        this.invalidateStale();
        this.storageFailed = false;
        this.failure = Failure.NONE;
        return this.transition;
    }

    synchronized void saved(Identity identity, long l, Selection selection, boolean bl) {
        if (!this.owns(identity) || l > this.transition) {
            return;
        }
        if (bl) {
            this.durable = selection;
            this.loaded = true;
            if (l == this.transition) {
                this.savedTransition = l;
                this.storageFailed = false;
                this.failure = Failure.NONE;
                this.publishIfSettled();
            }
        } else if (l == this.transition) {
            this.storageFailed = true;
            this.failure = Failure.STORAGE;
        }
    }

    private void publishIfSettled() {
        if (this.active && this.savedTransition == this.transition && this.committing == 0) {
            this.published = this.desired;
            this.fenceMask = 0;
        }
    }

    public synchronized Result<Boolean> observeContent(Identity identity, Conversation conversation, int n) {
        Failure failure = this.guard(identity, conversation);
        if (failure != Failure.NONE) {
            return this.fail(failure);
        }
        if (!PrivateReadingPolicy.validId(n)) {
            return this.fail(Failure.INVALID);
        }
        if (this.decision(identity, conversation) == ReceiptDecision.NATIVE) {
            return PrivateReadingPolicy.ok(false);
        }
        Pending pending = this.pending.get(conversation);
        if (pending != null && pending.ids.contains(n)) {
            return PrivateReadingPolicy.ok(true);
        }
        if (!this.capacity(conversation, 1, 128)) {
            return this.fail(Failure.CAPACITY);
        }
        if (pending == null) {
            pending = new Pending();
            this.pending.put(conversation, pending);
            this.retainKey(conversation);
        }
        pending.ids.add(n);
        ++this.contentCount;
        return PrivateReadingPolicy.ok(true);
    }

    public synchronized Result<SendAuthorization> registerSend(Identity identity, Conversation conversation, long l, Provenance provenance) {
        if (!this.owns(identity)) {
            return this.fail(Failure.INACTIVE);
        }
        if (conversation == null || conversation.mask() == 0 || l == 0L || provenance != Provenance.USER_MESSAGE && provenance != Provenance.TVE2_MESSAGE) {
            return this.fail(Failure.INVALID);
        }
        ReceiptDecision decision = this.decision(identity, conversation);
        if (decision != ReceiptDecision.WITHHOLD && decision != ReceiptDecision.NATIVE) {
            return this.fail(Failure.NOT_LOADED);
        }
        if (this.sends.containsKey(l)) {
            return this.fail(Failure.INVALID);
        }
        if (this.sends.size() >= 512 || !this.capacity(conversation, 0, 256)) {
            return this.fail(Failure.CAPACITY);
        }
        this.retainKey(conversation);
        SendAuthorization sendAuthorization = new SendAuthorization(this, conversation, l);
        this.sends.put(l, sendAuthorization);
        return PrivateReadingPolicy.ok(sendAuthorization);
    }

    public synchronized Result<ReleaseScope> acceptedSend(Identity identity, SendAuthorization sendAuthorization, int n) {
        if (!this.owns(identity)) {
            return this.fail(Failure.INACTIVE);
        }
        if (!PrivateReadingPolicy.validId(n) || sendAuthorization == null || this.sends.get(sendAuthorization.originalRandomId) != sendAuthorization
                || !this.validClassification(sendAuthorization.classificationRevision, sendAuthorization.conversation)) {
            return this.fail(Failure.INVALID);
        }
        this.sends.remove(sendAuthorization.originalRandomId);
        Result<ReleaseScope> result = this.currentDecision(sendAuthorization.conversation) == ReceiptDecision.NATIVE
                ? PrivateReadingPolicy.ok(null) : this.release(identity, sendAuthorization.conversation, n);
        this.releaseKey(sendAuthorization.conversation);
        return result;
    }

    public synchronized boolean failedSend(Identity identity, SendAuthorization sendAuthorization) {
        if (!this.owns(identity) || sendAuthorization == null || this.sends.get(sendAuthorization.originalRandomId) != sendAuthorization) {
            return false;
        }
        this.sends.remove(sendAuthorization.originalRandomId);
        this.releaseKey(sendAuthorization.conversation);
        return true;
    }

    public synchronized Result<ReleaseScope> manualRead(Identity identity, Conversation conversation, int n) {
        return this.release(identity, conversation, n);
    }

    private Result<ReleaseScope> release(Identity identity, Conversation conversation, int n) {
        Failure failure = this.guard(identity, conversation);
        if (failure != Failure.NONE) {
            return this.fail(failure);
        }
        if (this.decision(identity, conversation) != ReceiptDecision.WITHHOLD) {
            return this.fail(Failure.INVALID);
        }
        if (!PrivateReadingPolicy.validId(n) || conversation.mask() == 0) {
            return this.fail(Failure.INVALID);
        }
        /*
         * Invalid snapshots keep their IDs until a fresh action takes ownership.
         * A recovery cutoff prevents taking the same IDs twice while old native
         * callbacks still own the immutable snapshot. Only settled snapshots can
         * transfer their budget; outstanding requests and completions keep theirs.
         */
        Pending pending = this.pending.get(conversation);
        int recovered = 0;
        int reclaimIds = 0;
        int reclaimReleases = 0;
        for (Release release : this.releases.values()) {
            if (!release.recoverable(conversation)) continue;
            for (int id : release.scope.contentIds) {
                if (id > release.recoveredThrough && id <= n) ++recovered;
            }
            if (release.unowned() && release.recoveredBy(n)) {
                reclaimIds += release.scope.contentCount();
                ++reclaimReleases;
            }
        }
        int additionalIds = recovered - reclaimIds;
        if (this.releases.size() - reclaimReleases >= MAX_RELEASES
                || !this.capacity(conversation, additionalIds,
                        RELEASE_RETAINED_BYTES - reclaimReleases * RELEASE_RETAINED_BYTES + additionalIds * 96)) {
            return this.fail(Failure.CAPACITY);
        }
        TreeSet<Integer> ids = new TreeSet<Integer>();
        if (pending != null) ids.addAll(pending.ids.headSet(n, true));
        for (Release release : this.releases.values()) {
            if (!release.recoverable(conversation)) continue;
            for (int id : release.scope.contentIds) {
                if (id > release.recoveredThrough && id <= n) ids.add(id);
            }
        }
        int[] contentIds = new int[ids.size()];
        int index = 0;
        for (int id : ids) contentIds[index++] = id;
        this.retainKey(conversation);
        ReleaseScope scope = new ReleaseScope(this, conversation, n, contentIds);
        if (pending != null) {
            int removed = pending.ids.headSet(n, true).size();
            pending.ids.headSet(n, true).clear();
            this.contentCount -= removed;
            if (pending.ids.isEmpty()) {
                this.pending.remove(conversation);
                this.releaseKey(conversation);
            }
        }
        Iterator<Release> iterator = this.releases.values().iterator();
        while (iterator.hasNext()) {
            Release release = iterator.next();
            if (!release.recoverable(conversation)) continue;
            release.recoveredThrough = Math.max(release.recoveredThrough, n);
            if (!release.unowned() || !release.recoveredBy(release.recoveredThrough)) continue;
            this.contentCount -= release.scope.contentCount();
            this.releaseKey(release.scope.conversation);
            iterator.remove();
        }
        this.contentCount += contentIds.length;
        this.releases.put(scope.releaseId, new Release(scope));
        return PrivateReadingPolicy.ok(scope);
    }

    public synchronized Result<CommitReservation> reserveCommit(Identity identity, ReleaseScope releaseScope, long l) {
        Release release = this.find(releaseScope);
        if (!this.owns(identity)) {
            return this.fail(Failure.INACTIVE);
        }
        if (release == null || release.invalid || release.reservation != null || release.committed || !this.valid(releaseScope.stamp, releaseScope.conversation) || l < 0L) {
            return this.fail(Failure.INVALID);
        }
        release.reservation = new CommitReservation(releaseScope, l, ++this.sequence);
        ++this.committing;
        return PrivateReadingPolicy.ok(release.reservation);
    }

    public synchronized Result<Completion> completeCommit(Identity identity, CommitReservation commitReservation, boolean bl, int n, int n2, boolean bl2) {
        if (identity != this.identity || commitReservation == null) {
            return this.fail(Failure.INACTIVE);
        }
        Release release = this.find(commitReservation.scope);
        if (release == null || release.reservation != commitReservation) {
            return this.fail(Failure.INVALID);
        }
        if (n < 0 || n2 < -1 || bl2 && n2 < 0) {
            return this.fail(Failure.INVALID);
        }
        release.reservation = null;
        --this.committing;
        if (!this.active || !bl) {
            release.invalid = true;
            release.preserveContent = this.active;
            this.retireIfUnowned(release);
            this.publishIfSettled();
            return this.fail(this.active ? Failure.STORAGE : Failure.INACTIVE);
        }
        release.committed = true;
        release.completion = new Completion(commitReservation, n, n2, bl2);
        this.publishIfSettled();
        return PrivateReadingPolicy.ok(release.completion);
    }

    public synchronized Reconciliation reconcile(Identity identity, Completion completion, Projection projection, long l) {
        if (!this.owns(identity) || completion == null || projection == null) {
            return Reconciliation.REJECT;
        }
        Release release = this.find(completion.reservation.scope);
        int n = 1 << projection.ordinal();
        if (release == null || release.completion != completion || (completion.consumedProjections & n) != 0) {
            return Reconciliation.REJECT;
        }
        completion.consumedProjections |= n;
        return completion.completeBaseline && l == completion.reservation.baselineRevision ? Reconciliation.APPLY : Reconciliation.REFRESH;
    }

    public synchronized Result<RequestToken> reserveRequest(Identity identity, ReleaseScope releaseScope, boolean bl, int n, int n2) {
        if (!this.owns(identity)) {
            return this.fail(Failure.INACTIVE);
        }
        Release release = this.find(releaseScope);
        if (release == null || !release.committed || release.invalid || !this.valid(releaseScope.stamp, releaseScope.conversation) || n < 0 || n2 < 0 || n2 > 100 || n > releaseScope.contentCount() - n2 || !bl && n2 == 0 || bl && release.historyClaimed) {
            return this.fail(Failure.INVALID);
        }
        for (int i = n; i < n + n2; ++i) {
            if (!release.claimed[i]) continue;
            return this.fail(Failure.INVALID);
        }
        if (this.requests.size() >= 16 || !this.capacity(releaseScope.conversation, n2, 256 + n2 * 96)) {
            return this.fail(Failure.CAPACITY);
        }
        RequestToken requestToken = new RequestToken(this, releaseScope, releaseScope.conversation, bl ? releaseScope.historyCutoff : 0, n, n2);
        this.requests.put(requestToken, true);
        this.requestContentCount += n2;
        ++release.requests;
        if (bl) {
            release.historyClaimed = true;
        }
        for (int i = n; i < n + n2; ++i) {
            release.claimed[i] = true;
        }
        return PrivateReadingPolicy.ok(requestToken);
    }

    public synchronized Result<RequestToken> reserveNativeHistory(Identity identity, Conversation conversation, int n) {
        if (this.decision(identity, conversation) != ReceiptDecision.NATIVE || !PrivateReadingPolicy.validId(n)) {
            return this.fail(Failure.INVALID);
        }
        if (this.requests.size() >= 16 || !this.capacity(conversation, 0, 256)) {
            return this.fail(Failure.CAPACITY);
        }
        this.retainKey(conversation);
        RequestToken requestToken = new RequestToken(this, null, conversation, n, 0, 0);
        this.requests.put(requestToken, true);
        return PrivateReadingPolicy.ok(requestToken);
    }

    public synchronized Result<RequestToken> reserveNativeContent(Identity identity, Conversation conversation, int messageId) {
        if (conversation == null || this.decision(identity, conversation) != ReceiptDecision.NATIVE || !validId(messageId)) {
            return this.fail(Failure.INVALID);
        }
        if (this.requests.size() >= MAX_REQUESTS || !this.capacity(conversation, 1, 352)) {
            return this.fail(Failure.CAPACITY);
        }
        this.retainKey(conversation);
        RequestToken token = new RequestToken(this, null, conversation, 0, 0, 1, messageId);
        this.requests.put(token, true);
        ++this.requestContentCount;
        return ok(token);
    }

    public synchronized Result<RequestToken> reserveNativeMention(Identity identity, Conversation conversation, int mid) {
        for (RequestToken token : this.requests.keySet()) {
            if (token.nativeMention && token.nativeContentId == mid && token.conversation.equals(conversation)) {
                return this.fail(Failure.INVALID);
            }
        }
        Result<RequestToken> result = this.reserveNativeContent(identity, conversation, mid);
        if (result.accepted()) {
            result.value.nativeMention = true;
        }
        return result;
    }

    public synchronized boolean claimNativeMentionRefresh(Identity identity, RequestToken token) {
        if (!this.owns(identity) || token == null || !this.requests.containsKey(token)
                || !token.localProjectionPending) {
            return false;
        }
        for (RequestToken other : this.requests.keySet()) {
            if (other != token && other.refreshClaimed && other.conversation.equals(token.conversation)) {
                other.refreshDirty = true;
                return false;
            }
        }
        token.refreshClaimed = true;
        return true;
    }

    public synchronized boolean takeNativeMentionRefreshDirty(Identity identity, RequestToken token) {
        if (!this.owns(identity) || token == null || !this.requests.containsKey(token) || !token.refreshDirty) {
            return false;
        }
        token.refreshDirty = false;
        return true;
    }

    public synchronized boolean reserveNativeContentCommit(Identity identity, RequestToken token) {
        if (!this.owns(identity) || token == null || !this.requests.containsKey(token)
                || token.scope != null || !token.nativeMention || token.nativeContentId <= 0 || token.localStarted
                || token.transportBytes != 0 || token.handedOff || token.invalid
                || !this.valid(token.stamp, token.conversation)
                || this.currentDecision(token.conversation) != ReceiptDecision.NATIVE) {
            return false;
        }
        token.localStarted = true;
        token.localCommitting = true;
        token.localProjectionPending = true;
        ++this.committing;
        return true;
    }

    public synchronized boolean completeNativeContentCommit(Identity identity, RequestToken token,
                                                             boolean committed) {
        if (identity != this.identity || token == null || !this.requests.containsKey(token)
                || !token.localCommitting) {
            return false;
        }
        token.localCommitting = false;
        token.localCommitted = committed;
        if (!committed) {
            token.localProjectionPending = false;
        }
        --this.committing;
        this.publishIfSettled();
        return committed && this.active;
    }

    public synchronized boolean finishNativeContentProjection(Identity identity, RequestToken token) {
        if (identity != this.identity || token == null || !this.requests.containsKey(token)
                || token.localCommitting || !token.localProjectionPending) {
            return false;
        }
        token.localProjectionPending = false;
        if (token.transportFinished) {
            this.finishRequest(identity, token);
        }
        return true;
    }

    public synchronized boolean reserveNativeContentReconciliation(Identity identity, RequestToken token) {
        if (!this.owns(identity) || token == null || !this.requests.containsKey(token)
                || !token.localCommitted || !token.localProjectionPending || token.localCommitting) {
            return false;
        }
        token.localCommitting = true;
        ++this.committing;
        return true;
    }

    public synchronized void completeNativeContentReconciliation(Identity identity, RequestToken token) {
        if (identity == this.identity && token != null && this.requests.containsKey(token)
                && token.localCommitting) {
            token.localCommitting = false;
            --this.committing;
            this.publishIfSettled();
        }
    }

    public synchronized boolean claimTransport(Identity identity, RequestToken requestToken, int payloadBytes) {
        if (!this.owns(identity) || !this.requests.containsKey(requestToken)
                || requestToken.transportBytes != 0 || requestToken.invalid || requestToken.transportFinished
                || requestToken.localCommitting || requestToken.localStarted && !requestToken.localCommitted
                || !this.valid(requestToken.stamp, requestToken.conversation)
                || payloadBytes <= 0 || payloadBytes > MAX_RETAINED_BYTES) {
            return false;
        }
        if (!this.capacity(requestToken.conversation, 0, payloadBytes)) {
            this.failure = Failure.CAPACITY;
            return false;
        }
        requestToken.transportBytes = payloadBytes;
        this.transportPayloadBytes += payloadBytes;
        return true;
    }

    public synchronized boolean finishUnclaimedRequest(Identity identity, RequestToken requestToken) {
        return requestToken != null && requestToken.transportBytes == 0
                && this.finishRequest(identity, requestToken);
    }

    public synchronized boolean handOff(Identity identity, RequestToken requestToken) {
        if (!this.owns(identity) || !this.requests.containsKey(requestToken) || requestToken.handedOff
                || requestToken.invalid || requestToken.transportFinished || requestToken.localCommitting
                || requestToken.localStarted && !requestToken.localCommitted
                || !this.valid(requestToken.stamp, requestToken.conversation)) {
            return false;
        }
        if (requestToken.scope == null && this.currentDecision(requestToken.conversation) != ReceiptDecision.NATIVE) {
            return false;
        }
        Release release = this.find(requestToken.scope);
        if (requestToken.scope != null && (release == null || !release.committed || release.invalid)) {
            return false;
        }
        requestToken.handedOff = true;
        return true;
    }

    public synchronized boolean retryRequest(Identity identity, RequestToken requestToken) {
        if (!(this.owns(identity) && this.requests.containsKey(requestToken) && requestToken.handedOff && !requestToken.invalid && this.valid(requestToken.stamp, requestToken.conversation))) {
            return false;
        }
        requestToken.handedOff = false;
        return true;
    }

    public synchronized boolean needsCancellation(RequestToken requestToken) {
        return this.requests.containsKey(requestToken) && (requestToken.invalid || !this.active);
    }

    public synchronized RequestToken cancellationAt(int n) {
        for (RequestToken requestToken : this.requests.keySet()) {
            if (!requestToken.invalid && this.active || n-- != 0) continue;
            return requestToken;
        }
        return null;
    }

    public synchronized boolean finishRequest(Identity identity, RequestToken requestToken) {
        if (identity != this.identity || requestToken == null || requestToken.localCommitting
                || !this.requests.containsKey(requestToken)) {
            return false;
        }
        if (requestToken.localProjectionPending) {
            if (requestToken.transportFinished) {
                return false;
            }
            requestToken.transportFinished = true;
            this.transportPayloadBytes -= requestToken.transportBytes;
            requestToken.transportBytes = 0;
            return true;
        }
        this.requests.remove(requestToken);
        this.requestContentCount -= requestToken.count;
        this.transportPayloadBytes -= requestToken.transportBytes;
        Release release = this.find(requestToken.scope);
        if (release != null) {
            --release.requests;
            this.retireIfUnowned(release);
        } else if (requestToken.scope == null) {
            if (this.active && requestToken.invalid && requestToken.nativeContentId > 0) {
                Pending observed = this.pending.get(requestToken.conversation);
                if (observed == null) {
                    observed = new Pending();
                    this.pending.put(requestToken.conversation, observed);
                    this.retainKey(requestToken.conversation);
                }
                if (observed.ids.add(requestToken.nativeContentId)) {
                    ++this.contentCount;
                }
            }
            this.releaseKey(requestToken.conversation);
        }
        return true;
    }

    public synchronized boolean abandonRelease(Identity identity, ReleaseScope releaseScope) {
        if (identity != this.identity) {
            return false;
        }
        Release release = this.find(releaseScope);
        if (release == null || release.reservation != null || release.requests != 0) {
            return false;
        }
        release.invalid = true;
        release.preserveContent = this.active;
        release.completion = null;
        this.retireIfUnowned(release);
        return true;
    }

    public synchronized boolean finishRelease(Identity identity, ReleaseScope releaseScope) {
        if (identity != this.identity) {
            return false;
        }
        Release release = this.find(releaseScope);
        if (release == null || release.reservation != null || release.requests != 0) {
            return false;
        }
        release.completion = null;
        if (!release.invalid || !release.preserveContent || release.recoveredBy(release.recoveredThrough)) {
            this.removeRelease(release);
        }
        return true;
    }

    public synchronized Result<NotificationAction> registerNotification(Identity identity, Conversation conversation, int n) {
        Failure failure = this.guard(identity, conversation);
        if (failure != Failure.NONE) {
            return this.fail(failure);
        }
        if (!PrivateReadingPolicy.validId(n)) {
            return this.fail(Failure.INVALID);
        }
        if (this.decision(identity, conversation) != ReceiptDecision.WITHHOLD) {
            return this.fail(Failure.INVALID);
        }
        if (this.notifications.size() >= 256 || !this.capacity(conversation, 0, 384)) {
            return this.fail(Failure.CAPACITY);
        }
        this.retainKey(conversation);
        NotificationAction notificationAction = new NotificationAction(this, conversation, n);
        this.notifications.put(notificationAction.actionId, notificationAction);
        return PrivateReadingPolicy.ok(notificationAction);
    }

    public synchronized Result<ReleaseScope> consumeNotification(Identity identity, String string, Conversation conversation, int n) {
        if (!this.owns(identity)) {
            return this.fail(Failure.INACTIVE);
        }
        if (!this.matchesClassification(conversation)) {
            return this.fail(Failure.INVALID);
        }
        NotificationAction notificationAction = this.notifications.get(string);
        if (notificationAction == null || !notificationAction.conversation.equals(conversation) || notificationAction.cutoff != n || !this.valid(notificationAction.stamp, notificationAction.conversation)) {
            return this.fail(Failure.INVALID);
        }
        this.notifications.remove(string);
        Result<ReleaseScope> result = this.release(identity, notificationAction.conversation, notificationAction.cutoff);
        this.releaseKey(notificationAction.conversation);
        return result;
    }

    public synchronized boolean retireNotification(Identity identity, String string) {
        if (identity != this.identity) {
            return false;
        }
        NotificationAction notificationAction = this.notifications.remove(string);
        if (notificationAction == null) {
            return false;
        }
        this.releaseKey(notificationAction.conversation);
        return true;
    }

    public synchronized Result<SpecialConsent> beginSpecialConsent(Identity identity, Conversation conversation, int n) {
        Failure failure = this.guard(identity, conversation);
        if (failure != Failure.NONE) {
            return this.fail(failure);
        }
        if (!PrivateReadingPolicy.validId(n) || conversation.mask() == 0) {
            return this.fail(Failure.INVALID);
        }
        if (this.decision(identity, conversation) != ReceiptDecision.WITHHOLD) {
            return this.fail(Failure.INVALID);
        }
        if (this.special != null) {
            return this.fail(Failure.CAPACITY);
        }
        if (!this.capacity(conversation, 0, 256)) {
            return this.fail(Failure.CAPACITY);
        }
        this.retainKey(conversation);
        this.special = new SpecialConsent(this, conversation, n);
        return PrivateReadingPolicy.ok(this.special);
    }

    public synchronized boolean confirmSpecialConsent(Identity identity, SpecialConsent specialConsent, boolean bl) {
        if (!this.owns(identity) || this.special == null || this.special != specialConsent) {
            return false;
        }
        if (!bl || !this.valid(specialConsent.stamp, specialConsent.conversation)) {
            this.clearSpecial();
            return false;
        }
        specialConsent.confirmed = true;
        return true;
    }

    public synchronized boolean consumeSpecialConsent(Identity identity, SpecialConsent specialConsent, Conversation conversation, int n) {
        if (!(this.owns(identity) && this.matchesClassification(conversation)
                && this.special != null && this.special == specialConsent && specialConsent.confirmed && specialConsent.conversation.equals(conversation) && specialConsent.messageId == n && this.valid(specialConsent.stamp, conversation))) {
            return false;
        }
        this.clearSpecial();
        return true;
    }

    public synchronized void cancelSpecialConsent(Identity identity, SpecialConsent specialConsent) {
        if (identity == this.identity && this.special == specialConsent) {
            this.clearSpecial();
        }
    }

    public synchronized void discardPending(Identity identity, Conversation conversation) {
        if (!this.owns(identity)) {
            return;
        }
        Pending pending = this.pending.remove(conversation);
        if (pending != null) {
            this.contentCount -= pending.ids.size();
            this.releaseKey(conversation);
        }
    }

    public synchronized void invalidate(Identity identity) {
        if (!this.owns(identity)) {
            return;
        }
        this.active = false;
        ++this.masterGeneration;
        this.invalidateStale();
    }

    private void invalidateStale() {
        Iterator<Map.Entry<Conversation, Pending>> iterator2 = this.pending.entrySet().iterator();
        while (iterator2.hasNext()) {
            Map.Entry<Conversation, Pending> object = iterator2.next();
            if (this.active) continue;
            this.contentCount -= ((Pending)object.getValue()).ids.size();
            this.releaseKey((Conversation)object.getKey());
            iterator2.remove();
        }
        Iterator<SendAuthorization> sendIterator = this.sends.values().iterator();
        while (sendIterator.hasNext()) {
            SendAuthorization send = sendIterator.next();
            if (this.active && this.validClassification(send.classificationRevision, send.conversation)) continue;
            this.releaseKey(send.conversation);
            sendIterator.remove();
        }
        Iterator<NotificationAction> iterator = this.notifications.values().iterator();
        while (iterator.hasNext()) {
            NotificationAction action = iterator.next();
            if (this.active && this.valid(action.stamp, action.conversation)) continue;
            this.releaseKey(action.conversation);
            iterator.remove();
        }
        if (!(this.special == null || this.active && this.valid(this.special.stamp, this.special.conversation))) {
            this.clearSpecial();
        }
        for (RequestToken object2 : this.requests.keySet()) {
            if (this.active && this.valid(object2.stamp, object2.conversation)) continue;
            object2.invalid = true;
        }
        Iterator<Release> releaseIterator = this.releases.values().iterator();
        while (releaseIterator.hasNext()) {
            Release release = releaseIterator.next();
            if (this.active && this.valid(release.scope.stamp, release.scope.conversation)) continue;
            release.invalid = true;
            release.preserveContent = this.active;
            if (!release.unowned() || release.preserveContent && !release.recoveredBy(release.recoveredThrough)) continue;
            this.contentCount -= release.scope.contentCount();
            this.releaseKey(release.scope.conversation);
            releaseIterator.remove();
        }
    }

    private void clearSpecial() {
        if (this.special != null) {
            this.releaseKey(this.special.conversation);
        }
        this.special = null;
    }

    private void retireIfUnowned(Release release) {
        if (release.invalid && release.unowned()
                && (!release.preserveContent || !this.active || release.recoveredBy(release.recoveredThrough))) {
            this.removeRelease(release);
        }
    }

    private void removeRelease(Release release) {
        this.releases.remove(release.scope.releaseId);
        this.contentCount -= release.scope.contentCount();
        this.releaseKey(release.scope.conversation);
    }

    private Release find(ReleaseScope releaseScope) {
        if (releaseScope == null || releaseScope.identity != this.identity) {
            return null;
        }
        Release release = this.releases.get(releaseScope.releaseId);
        return release != null && release.scope == releaseScope ? release : null;
    }

    private ReceiptDecision currentDecision(Conversation conversation) {
        Classification classification = this.keys.get(conversation);
        int mask = classification == null ? conversation.mask() : Conversation.mask(classification.category);
        if (mask == 0) return ReceiptDecision.NATIVE;
        if (!this.loaded) return ReceiptDecision.NOT_LOADED;
        return ((this.published.mask() | this.durable.mask() | this.fenceMask) & mask) != 0
                ? ReceiptDecision.WITHHOLD : ReceiptDecision.NATIVE;
    }

    private boolean matchesClassification(Conversation conversation) {
        if (conversation == null) return false;
        Classification classification = this.keys.get(conversation);
        if (classification == null) return false;
        if (classification.category == conversation.category) return true;
        classification.revision = ++this.sequence;
        this.invalidateStale();
        return false;
    }

    private boolean validClassification(long revision, Conversation conversation) {
        Classification classification = this.keys.get(conversation);
        return classification != null && classification.category == conversation.category
                && revision == classification.revision;
    }

    private boolean valid(Stamp stamp, Conversation conversation) {
        int mask = conversation.mask();
        return this.validClassification(stamp.classification, conversation)
                && (mask == 0 || (stamp.master == this.masterGeneration
                && ((mask & 1) == 0 || stamp.privateChats == this.privateGeneration)
                && ((mask & 2) == 0 || stamp.groups == this.groupGeneration)
                && ((mask & 4) == 0 || stamp.channels == this.channelGeneration)
                && ((mask & 8) == 0 || stamp.bots == this.botGeneration)));
    }

    private Failure guard(Identity identity, Conversation conversation) {
        if (!this.owns(identity)) {
            return Failure.INACTIVE;
        }
        if (conversation == null) {
            return Failure.INVALID;
        }
        return this.loaded ? Failure.NONE : Failure.NOT_LOADED;
    }

    private static boolean validId(int n) {
        return n > 0 && n < Integer.MAX_VALUE;
    }

    private boolean capacity(Conversation conversation, int n, int n2) {
        return this.contentCount + this.requestContentCount + n <= MAX_CONTENT_IDS && (this.keys.containsKey(conversation) || this.keys.size() < 1024) && this.retainedBytes() + n2 + (this.keys.containsKey(conversation) ? 0 : 256) <= 0x200000;
    }

    private int retainedBytes() {
        return 4096 + this.transportPayloadBytes + this.keys.size() * 256 + (this.contentCount + this.requestContentCount) * 96 + this.sends.size() * 256 + this.releases.size() * RELEASE_RETAINED_BYTES + this.requests.size() * 256 + this.notifications.size() * 384 + (this.special == null ? 0 : 256);
    }

    private void retainKey(Conversation conversation) {
        Classification classification = this.keys.get(conversation);
        if (classification == null) {
            classification = new Classification(conversation.category, ++this.sequence);
            this.keys.put(conversation, classification);
        }
        ++classification.references;
    }

    private void releaseKey(Conversation conversation) {
        Classification classification = this.keys.get(conversation);
        if (classification == null) {
            throw new IllegalStateException("Unowned conversation");
        }
        if (--classification.references == 0) this.keys.remove(conversation);
    }

    private static final class Classification {
        Category category;
        long revision;
        int references;

        Classification(Category category, long revision) {
            this.category = category;
            this.revision = revision;
        }
    }

    private static <T> Result<T> ok(T t) {
        return new Result(t, Failure.NONE);
    }

    private <T> Result<T> fail(Failure failure) {
        if (failure == Failure.CAPACITY || failure == Failure.STORAGE) {
            this.failure = failure;
        }
        return new Result(null, failure);
    }

    public static final class Identity {
        public final Environment environment;
        public final long userId;
        public final UUID lifetime;

        private Identity(Environment environment, long l) {
            this.environment = Objects.requireNonNull(environment);
            if (l <= 0L) {
                throw new IllegalArgumentException("Missing stable user identity");
            }
            this.userId = l;
            this.lifetime = UUID.randomUUID();
        }
    }

    public static enum Failure {
        NONE,
        INACTIVE,
        NOT_LOADED,
        INVALID,
        CAPACITY,
        STORAGE;

    }

    public static final class Selection {
        public final boolean master;
        public final boolean privateChats;
        public final boolean groups;
        public final boolean channels;
        public final boolean bots;

        public Selection(boolean bl, boolean bl2, boolean bl3, boolean bl4) {
            this(bl, bl2, bl3, bl4, bl2);
        }

        public Selection(boolean master, boolean privateChats, boolean groups, boolean channels, boolean bots) {
            this.master = master;
            this.privateChats = privateChats;
            this.groups = groups;
            this.channels = channels;
            this.bots = bots;
        }

        public static Selection off() {
            return new Selection(false, true, true, true);
        }

        private int mask() {
            return this.master ? (this.privateChats ? 1 : 0) | (this.groups ? 2 : 0) | (this.channels ? 4 : 0) | (this.bots ? 8 : 0) : 0;
        }

        public boolean equals(Object object) {
            if (!(object instanceof Selection)) {
                return false;
            }
            Selection selection = (Selection)object;
            return this.master == selection.master && this.privateChats == selection.privateChats && this.groups == selection.groups && this.channels == selection.channels && this.bots == selection.bots;
        }

        public int hashCode() {
            return (this.master ? 16 : 0) | (this.privateChats ? 1 : 0) | (this.groups ? 2 : 0) | (this.channels ? 4 : 0) | (this.bots ? 8 : 0);
        }
    }

    public static enum Environment {
        PRODUCTION,
        TEST;

    }

    public static final class Status {
        public final Identity identity;
        public final boolean active;
        public final boolean loaded;
        public final Selection published;
        public final Selection durable;
        public final Selection desired;
        public final long transition;
        public final boolean saved;
        public final boolean unsavedProtection;
        public final boolean storageFailed;
        public final Failure failure;
        public final int retainedContentIds;
        public final int retainedBytes;

        private Status(PrivateReadingPolicy privateReadingPolicy) {
            this.identity = privateReadingPolicy.identity;
            this.active = privateReadingPolicy.active;
            this.loaded = privateReadingPolicy.loaded;
            this.published = privateReadingPolicy.published;
            this.durable = privateReadingPolicy.durable;
            this.desired = privateReadingPolicy.desired;
            this.transition = privateReadingPolicy.transition;
            this.saved = this.loaded && privateReadingPolicy.savedTransition == this.transition && privateReadingPolicy.committing == 0;
            this.unsavedProtection = (privateReadingPolicy.fenceMask & ~this.durable.mask()) != 0;
            this.storageFailed = privateReadingPolicy.storageFailed;
            this.failure = privateReadingPolicy.failure;
            this.retainedContentIds = privateReadingPolicy.contentCount + privateReadingPolicy.requestContentCount;
            this.retainedBytes = privateReadingPolicy.retainedBytes();
        }
    }

    public static enum ReceiptDecision {
        NATIVE,
        WITHHOLD,
        NOT_LOADED,
        INACTIVE;

    }

    public static final class Conversation {
        public final long dialogId;
        public final long rootId;
        public final long subpeerId;
        public final Category category;

        public Conversation(long l, long l2, long l3, Category category) {
            if (l == 0L || l2 < 0L || l2 != 0L && l3 != 0L) {
                throw new IllegalArgumentException("Invalid conversation identity");
            }
            this.dialogId = l;
            this.rootId = l2;
            this.subpeerId = l3;
            this.category = Objects.requireNonNull(category);
        }

        public static Category classify(boolean bl, boolean bl2, boolean bl3, boolean bl4, boolean bl5) {
            if (bl2) {
                return Category.SECRET;
            }
            if (bl) {
                return Category.SELF;
            }
            if (bl3) {
                return Category.PRIVATE;
            }
            if (!bl4) {
                return Category.UNKNOWN_GROUP_OR_CHANNEL;
            }
            return bl5 ? Category.CHANNEL : Category.GROUP;
        }

        private int mask() {
            return mask(this.category);
        }

        private static int mask(Category category) {
            switch (category) {
                case BOT: {
                    return 8;
                }
                case UNKNOWN_PRIVATE_OR_BOT: {
                    return 9;
                }
                case PRIVATE: {
                    return 1;
                }
                case GROUP: {
                    return 2;
                }
                case CHANNEL: {
                    return 4;
                }
                case UNKNOWN_GROUP_OR_CHANNEL: {
                    return 6;
                }
            }
            return 0;
        }

        public boolean equals(Object object) {
            if (!(object instanceof Conversation)) {
                return false;
            }
            Conversation conversation = (Conversation)object;
            return this.dialogId == conversation.dialogId && this.rootId == conversation.rootId && this.subpeerId == conversation.subpeerId;
        }

        public int hashCode() {
            return Objects.hash(new Object[]{this.dialogId, this.rootId, this.subpeerId});
        }
    }

    public static final class Result<T> {
        public final T value;
        public final Failure failure;

        private Result(T t, Failure failure) {
            this.value = t;
            this.failure = failure;
        }

        public boolean accepted() {
            return this.failure == Failure.NONE;
        }
    }

    private static final class Pending {
        final TreeSet<Integer> ids = new TreeSet();

        private Pending() {
        }
    }

    public static enum Provenance {
        USER_MESSAGE,
        TVE2_MESSAGE,
        SERVICE,
        SCHEDULED,
        QUICK_REPLY_TEMPLATE,
        BUSINESS_UPDATE,
        TVE2_CONTROL,
        RESTORED;

    }

    public static final class SendAuthorization {
        public final Identity identity;
        public final Conversation conversation;
        public final long originalRandomId;

        private final long classificationRevision;

        private SendAuthorization(PrivateReadingPolicy policy, Conversation conversation, long l) {
            this.identity = policy.identity;
            this.classificationRevision = policy.keys.get(conversation).revision;
            this.conversation = conversation;
            this.originalRandomId = l;
        }
    }

    public static final class ReleaseScope {
        public final Identity identity;
        public final long releaseId;
        public final Conversation conversation;
        public final int historyCutoff;
        private final Stamp stamp;
        private final int[] contentIds;

        private ReleaseScope(PrivateReadingPolicy privateReadingPolicy, Conversation conversation, int n, int[] nArray) {
            this.identity = privateReadingPolicy.identity;
            this.releaseId = ++privateReadingPolicy.sequence;
            this.conversation = conversation;
            this.historyCutoff = n;
            this.stamp = new Stamp(privateReadingPolicy, conversation);
            this.contentIds = nArray;
        }

        public int contentCount() {
            return this.contentIds.length;
        }

        public int contentIdAt(int n) {
            return this.contentIds[n];
        }
    }

    private static final class Release {
        final ReleaseScope scope;
        final boolean[] claimed;
        boolean historyClaimed;
        boolean invalid;
        boolean preserveContent;
        int recoveredThrough;
        boolean committed;
        CommitReservation reservation;
        Completion completion;
        int requests;

        Release(ReleaseScope releaseScope) {
            this.scope = releaseScope;
            this.claimed = new boolean[releaseScope.contentCount()];
        }

        boolean unowned() {
            return this.reservation == null && this.requests == 0 && this.completion == null;
        }

        boolean recoverable(Conversation conversation) {
            return this.invalid && this.preserveContent && this.scope.conversation.equals(conversation);
        }

        boolean recoveredBy(int cutoff) {
            return this.scope.contentCount() == 0
                    || this.scope.contentIdAt(this.scope.contentCount() - 1) <= Math.max(cutoff, this.recoveredThrough);
        }
    }

    public static final class CommitReservation {
        public final Identity identity;
        public final ReleaseScope scope;
        public final long reconciliationId;
        public final long baselineRevision;

        private CommitReservation(ReleaseScope releaseScope, long l, long l2) {
            this.identity = releaseScope.identity;
            this.scope = releaseScope;
            this.baselineRevision = l;
            this.reconciliationId = l2;
        }
    }

    private static final class Stamp {
        final long master;
        final long privateChats;
        final long groups;
        final long channels;
        final long bots;
        final long classification;

        Stamp(PrivateReadingPolicy privateReadingPolicy, Conversation conversation) {
            this.master = privateReadingPolicy.masterGeneration;
            this.privateChats = privateReadingPolicy.privateGeneration;
            this.groups = privateReadingPolicy.groupGeneration;
            this.channels = privateReadingPolicy.channelGeneration;
            this.bots = privateReadingPolicy.botGeneration;
            this.classification = privateReadingPolicy.keys.get(conversation).revision;
        }
    }

    public static final class Completion {
        public final CommitReservation reservation;
        public final int changedRows;
        public final int remainingUnread;
        public final boolean completeBaseline;
        private int consumedProjections;

        private Completion(CommitReservation commitReservation, int n, int n2, boolean bl) {
            this.reservation = commitReservation;
            this.changedRows = n;
            this.remainingUnread = n2;
            this.completeBaseline = bl;
        }
    }

    public static enum Reconciliation {
        APPLY,
        REFRESH,
        REJECT;

    }

    public static enum Projection {
        MESSAGE_FLAGS,
        COUNTERS,
        NOTIFICATIONS;

    }

    public static final class RequestToken {
        public final Identity identity;
        public final Conversation conversation;
        public final int historyCutoff;
        private final ReleaseScope scope;
        private final Stamp stamp;
        private final int offset;
        private final int count;
        private final int nativeContentId;
        private int transportBytes;
        private boolean handedOff;
        private boolean invalid;
        private boolean localStarted;
        private boolean localCommitting;
        private boolean localCommitted;
        private boolean localProjectionPending;
        private boolean transportFinished;
        private boolean nativeMention;
        private boolean refreshClaimed;
        private boolean refreshDirty;

        private RequestToken(PrivateReadingPolicy privateReadingPolicy, ReleaseScope releaseScope, Conversation conversation, int n, int n2, int n3) {
            this(privateReadingPolicy, releaseScope, conversation, n, n2, n3, 0);
        }

        private RequestToken(PrivateReadingPolicy privateReadingPolicy, ReleaseScope releaseScope, Conversation conversation,
                             int n, int n2, int n3, int nativeContentId) {
            this.nativeContentId = nativeContentId;
            this.identity = privateReadingPolicy.identity;
            this.scope = releaseScope;
            this.conversation = conversation;
            this.historyCutoff = n;
            this.stamp = new Stamp(privateReadingPolicy, conversation);
            this.offset = n2;
            this.count = n3;
        }

        public int contentCount() {
            return this.count;
        }

        public int contentIdAt(int n) {
            if (n < 0 || n >= this.count) {
                throw new IndexOutOfBoundsException();
            }
            return this.scope == null ? this.nativeContentId : this.scope.contentIdAt(this.offset + n);
        }
    }

    public static final class NotificationAction {
        public final Identity identity;
        public final String actionId;
        public final Conversation conversation;
        public final int cutoff;
        private final Stamp stamp;

        private NotificationAction(PrivateReadingPolicy privateReadingPolicy, Conversation conversation, int n) {
            this.identity = privateReadingPolicy.identity;
            this.actionId = UUID.randomUUID().toString();
            this.conversation = conversation;
            this.cutoff = n;
            this.stamp = new Stamp(privateReadingPolicy, conversation);
        }
    }

    public static final class SpecialConsent {
        public final Identity identity;
        public final Conversation conversation;
        public final int messageId;
        private final Stamp stamp;
        private boolean confirmed;

        private SpecialConsent(PrivateReadingPolicy privateReadingPolicy, Conversation conversation, int n) {
            this.identity = privateReadingPolicy.identity;
            this.conversation = conversation;
            this.messageId = n;
            this.stamp = new Stamp(privateReadingPolicy, conversation);
        }
    }

    public static enum Category {
        PRIVATE,
        BOT,
        UNKNOWN_PRIVATE_OR_BOT,
        GROUP,
        CHANNEL,
        UNKNOWN_GROUP_OR_CHANNEL,
        SELF,
        SECRET;

    }
}
