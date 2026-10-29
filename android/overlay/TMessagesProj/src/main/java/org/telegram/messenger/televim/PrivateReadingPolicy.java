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
    private long sequence;
    private int contentCount;
    private int committing;
    private final Map<Conversation, Pending> pending = new HashMap<Conversation, Pending>();
    private final Map<Conversation, Integer> keys = new HashMap<Conversation, Integer>();
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
        if (conversation.mask() == 0) {
            return ReceiptDecision.NATIVE;
        }
        if (!this.loaded) {
            return ReceiptDecision.NOT_LOADED;
        }
        return ((this.published.mask() | this.durable.mask() | this.fenceMask) & conversation.mask()) != 0 ? ReceiptDecision.WITHHOLD : ReceiptDecision.NATIVE;
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
        if (this.decision(identity, conversation) != ReceiptDecision.WITHHOLD) {
            return this.fail(Failure.INVALID);
        }
        if (this.sends.containsKey(l)) {
            return this.fail(Failure.INVALID);
        }
        if (this.sends.size() >= 512 || !this.capacity(conversation, 0, 256)) {
            return this.fail(Failure.CAPACITY);
        }
        SendAuthorization sendAuthorization = new SendAuthorization(this.identity, conversation, l);
        this.sends.put(l, sendAuthorization);
        this.retainKey(conversation);
        return PrivateReadingPolicy.ok(sendAuthorization);
    }

    public synchronized Result<ReleaseScope> acceptedSend(Identity identity, SendAuthorization sendAuthorization, int n) {
        if (!this.owns(identity)) {
            return this.fail(Failure.INACTIVE);
        }
        if (!PrivateReadingPolicy.validId(n) || sendAuthorization == null || this.sends.get(sendAuthorization.originalRandomId) != sendAuthorization) {
            return this.fail(Failure.INVALID);
        }
        this.sends.remove(sendAuthorization.originalRandomId);
        this.releaseKey(sendAuthorization.conversation);
        if (this.decision(identity, sendAuthorization.conversation) == ReceiptDecision.NATIVE) {
            return PrivateReadingPolicy.ok(null);
        }
        return this.release(identity, sendAuthorization.conversation, n);
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
        if (this.releases.size() >= 64 || !this.capacity(conversation, 0, 512)) {
            return this.fail(Failure.CAPACITY);
        }
        Pending pending = this.pending.get(conversation);
        int n2 = pending == null ? 0 : pending.ids.headSet(n, true).size();
        int[] nArray = new int[n2];
        if (pending != null) {
            int n3;
            Iterator<Integer> object = pending.ids.iterator();
            int n4 = 0;
            while (object.hasNext() && (n3 = ((Integer)object.next()).intValue()) <= n) {
                nArray[n4++] = n3;
                object.remove();
            }
            if (pending.ids.isEmpty()) {
                this.pending.remove(conversation);
                this.releaseKey(conversation);
            }
        }
        ReleaseScope object = new ReleaseScope(this, conversation, n, nArray);
        this.releases.put(object.releaseId, new Release(object));
        this.retainKey(conversation);
        return PrivateReadingPolicy.ok(object);
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
        if (this.requests.size() >= 16 || !this.capacity(releaseScope.conversation, 0, 256 + n2 * 96)) {
            return this.fail(Failure.CAPACITY);
        }
        RequestToken requestToken = new RequestToken(this, releaseScope, releaseScope.conversation, bl ? releaseScope.historyCutoff : 0, n, n2);
        this.requests.put(requestToken, true);
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
        RequestToken requestToken = new RequestToken(this, null, conversation, n, 0, 0);
        this.requests.put(requestToken, true);
        this.retainKey(conversation);
        return PrivateReadingPolicy.ok(requestToken);
    }

    public synchronized boolean handOff(Identity identity, RequestToken requestToken) {
        if (!this.owns(identity) || !this.requests.containsKey(requestToken) || requestToken.handedOff || requestToken.invalid || !this.valid(requestToken.stamp, requestToken.conversation)) {
            return false;
        }
        if (requestToken.scope == null && this.decision(identity, requestToken.conversation) != ReceiptDecision.NATIVE) {
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
        if (identity != this.identity || this.requests.remove(requestToken) == null) {
            return false;
        }
        Release release = this.find(requestToken.scope);
        if (release != null) {
            --release.requests;
            this.retireIfUnowned(release);
        } else if (requestToken.scope == null) {
            this.releaseKey(requestToken.conversation);
        }
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
        this.removeRelease(release);
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
        NotificationAction notificationAction = new NotificationAction(this, conversation, n);
        this.notifications.put(notificationAction.actionId, notificationAction);
        this.retainKey(conversation);
        return PrivateReadingPolicy.ok(notificationAction);
    }

    public synchronized Result<ReleaseScope> consumeNotification(Identity identity, String string, Conversation conversation, int n) {
        if (!this.owns(identity)) {
            return this.fail(Failure.INACTIVE);
        }
        NotificationAction notificationAction = this.notifications.get(string);
        if (notificationAction == null || !notificationAction.conversation.equals(conversation) || notificationAction.cutoff != n || !this.valid(notificationAction.stamp, notificationAction.conversation)) {
            return this.fail(Failure.INVALID);
        }
        this.notifications.remove(string);
        this.releaseKey(notificationAction.conversation);
        return this.release(identity, notificationAction.conversation, notificationAction.cutoff);
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
        this.special = new SpecialConsent(this, conversation, n);
        this.retainKey(conversation);
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
        if (!(this.owns(identity) && this.special != null && this.special == specialConsent && specialConsent.confirmed && specialConsent.conversation.equals(conversation) && specialConsent.messageId == n && this.valid(specialConsent.stamp, conversation))) {
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
        for (SendAuthorization sendAuthorization : this.sends.values()) {
            this.releaseKey(sendAuthorization.conversation);
        }
        this.sends.clear();
    }

    private void invalidateStale() {
        Iterator<Map.Entry<Conversation, Pending>> iterator2 = this.pending.entrySet().iterator();
        while (iterator2.hasNext()) {
            Map.Entry<Conversation, Pending> object = iterator2.next();
            if (this.active && this.decision(this.identity, (Conversation)object.getKey()) != ReceiptDecision.NATIVE) continue;
            this.contentCount -= ((Pending)object.getValue()).ids.size();
            this.releaseKey((Conversation)object.getKey());
            iterator2.remove();
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
            if (release.reservation != null || release.requests != 0 || release.completion != null) continue;
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
        if (release.invalid && release.reservation == null && release.requests == 0 && (release.completion == null || !this.active)) {
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

    private boolean valid(Stamp stamp, Conversation conversation) {
        int n = conversation.mask();
        return !(stamp.master != this.masterGeneration || (n & 1) != 0 && stamp.privateChats != this.privateGeneration || (n & 2) != 0 && stamp.groups != this.groupGeneration || (n & 4) != 0 && stamp.channels != this.channelGeneration);
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
        return this.contentCount + n <= 8192 && (this.keys.containsKey(conversation) || this.keys.size() < 1024) && this.retainedBytes() + n2 + (this.keys.containsKey(conversation) ? 0 : 256) <= 0x200000;
    }

    private int retainedBytes() {
        return 4096 + this.keys.size() * 256 + this.contentCount * 96 + this.sends.size() * 256 + this.releases.size() * 512 + this.requests.size() * 256 + this.notifications.size() * 384 + (this.special == null ? 0 : 256);
    }

    private void retainKey(Conversation conversation) {
        Integer n = this.keys.get(conversation);
        this.keys.put(conversation, n == null ? 1 : n + 1);
    }

    private void releaseKey(Conversation conversation) {
        Integer n = this.keys.get(conversation);
        if (n == null) {
            throw new IllegalStateException("Unowned conversation");
        }
        if (n == 1) {
            this.keys.remove(conversation);
        } else {
            this.keys.put(conversation, n - 1);
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

        public Selection(boolean bl, boolean bl2, boolean bl3, boolean bl4) {
            this.master = bl;
            this.privateChats = bl2;
            this.groups = bl3;
            this.channels = bl4;
        }

        public static Selection off() {
            return new Selection(false, true, true, true);
        }

        private int mask() {
            return this.master ? (this.privateChats ? 1 : 0) | (this.groups ? 2 : 0) | (this.channels ? 4 : 0) : 0;
        }

        public boolean equals(Object object) {
            if (!(object instanceof Selection)) {
                return false;
            }
            Selection selection = (Selection)object;
            return this.master == selection.master && this.privateChats == selection.privateChats && this.groups == selection.groups && this.channels == selection.channels;
        }

        public int hashCode() {
            return (this.master ? 8 : 0) | (this.privateChats ? 1 : 0) | (this.groups ? 2 : 0) | (this.channels ? 4 : 0);
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
            this.retainedContentIds = privateReadingPolicy.contentCount;
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
            switch (this.category) {
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
            return this.dialogId == conversation.dialogId && this.rootId == conversation.rootId && this.subpeerId == conversation.subpeerId && this.category == conversation.category;
        }

        public int hashCode() {
            return Objects.hash(new Object[]{this.dialogId, this.rootId, this.subpeerId, this.category});
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

        private SendAuthorization(Identity identity, Conversation conversation, long l) {
            this.identity = identity;
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
            this.stamp = new Stamp(privateReadingPolicy);
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
        boolean committed;
        CommitReservation reservation;
        Completion completion;
        int requests;

        Release(ReleaseScope releaseScope) {
            this.scope = releaseScope;
            this.claimed = new boolean[releaseScope.contentCount()];
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

        Stamp(PrivateReadingPolicy privateReadingPolicy) {
            this.master = privateReadingPolicy.masterGeneration;
            this.privateChats = privateReadingPolicy.privateGeneration;
            this.groups = privateReadingPolicy.groupGeneration;
            this.channels = privateReadingPolicy.channelGeneration;
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
        private boolean handedOff;
        private boolean invalid;

        private RequestToken(PrivateReadingPolicy privateReadingPolicy, ReleaseScope releaseScope, Conversation conversation, int n, int n2, int n3) {
            this.identity = privateReadingPolicy.identity;
            this.scope = releaseScope;
            this.conversation = conversation;
            this.historyCutoff = n;
            this.stamp = new Stamp(privateReadingPolicy);
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
            return this.scope.contentIdAt(this.offset + n);
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
            this.stamp = new Stamp(privateReadingPolicy);
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
            this.stamp = new Stamp(privateReadingPolicy);
        }
    }

    public static enum Category {
        PRIVATE,
        GROUP,
        CHANNEL,
        UNKNOWN_GROUP_OR_CHANNEL,
        SELF,
        SECRET;

    }
}
