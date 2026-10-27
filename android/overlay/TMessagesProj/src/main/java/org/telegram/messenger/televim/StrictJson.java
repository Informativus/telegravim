package org.telegram.messenger.televim;

import android.util.JsonReader;
import android.util.JsonToken;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.io.StringReader;
import java.math.BigDecimal;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Pattern;

final class StrictJson {
    private static final Pattern NUMBER = Pattern.compile("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?");
    private int nodes;
    private final AtomicBoolean cancelled;

    static JSONObject parse(byte[] bytes, int limit) throws Exception {
        return parse(bytes, limit, null);
    }

    static JSONObject parse(byte[] bytes, int limit, AtomicBoolean cancelled) throws Exception {
        OfflineTransfer.checkCancelled(cancelled);
        if (bytes.length > limit) {
            throw new IOException("JSON size limit exceeded");
        }
        String text = StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes)).toString();
        validateLexemes(text, cancelled);
        try (JsonReader reader = new JsonReader(new StringReader(text))) {
            reader.setLenient(false);
            Object result = new StrictJson(cancelled).read(reader, 1);
            if (!(result instanceof JSONObject) || reader.peek() != JsonToken.END_DOCUMENT) {
                throw new IOException("Invalid JSON document");
            }
            return (JSONObject) result;
        }
    }

    private Object read(JsonReader reader, int depth) throws IOException, JSONException {
        OfflineTransfer.checkCancelled(cancelled);
        JsonToken token = reader.peek();
        if ((depth > 32 && (token == JsonToken.BEGIN_OBJECT || token == JsonToken.BEGIN_ARRAY)) || ++nodes > 150000) {
            throw new IOException("JSON complexity limit exceeded");
        }
        switch (token) {
            case BEGIN_OBJECT: {
                JSONObject result = new JSONObject();
                Set<String> names = new HashSet<>();
                reader.beginObject();
                while (reader.hasNext()) {
                    String name = checkedString(reader.nextName());
                    if (!names.add(name)) {
                        throw new IOException("Duplicate JSON key");
                    }
                    result.put(name, read(reader, depth + 1));
                }
                reader.endObject();
                return result;
            }
            case BEGIN_ARRAY: {
                JSONArray result = new JSONArray();
                reader.beginArray();
                while (reader.hasNext()) {
                    result.put(read(reader, depth + 1));
                }
                reader.endArray();
                return result;
            }
            case STRING:
                return checkedString(reader.nextString());
            case NUMBER: {
                String number = reader.nextString();
                if (number.length() > 128 || !NUMBER.matcher(number).matches()) {
                    throw new IOException("Invalid JSON number");
                }
                BigDecimal result = new BigDecimal(number);
                if (Math.abs((long) result.scale()) > 10000) {
                    throw new IOException("JSON number limit exceeded");
                }
                return result;
            }
            case BOOLEAN:
                return reader.nextBoolean();
            case NULL:
                reader.nextNull();
                return JSONObject.NULL;
            default:
                throw new IOException("Invalid JSON token");
        }
    }

    private static void validateLexemes(String text, AtomicBoolean cancelled) throws IOException {
        int tokens = 0;
        for (int i = 0; i < text.length();) {
            OfflineTransfer.checkCancelled(cancelled);
            char c = text.charAt(i++);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}'
                    || c == '[' || c == ']' || c == ':' || c == ',') {
                continue;
            }
            if (++tokens > 150000) {
                throw new IOException("JSON complexity limit exceeded");
            }
            if (c == '"') {
                boolean closed = false;
                while (i < text.length()) {
                    if ((i & 1023) == 0) {
                        OfflineTransfer.checkCancelled(cancelled);
                    }
                    c = text.charAt(i++);
                    if (c < 0x20) {
                        throw new IOException("Unescaped JSON control character");
                    }
                    if (c == '"') {
                        closed = true;
                        break;
                    }
                    if (c == '\\') {
                        if (i == text.length()) {
                            throw new IOException("Incomplete JSON escape");
                        }
                        c = text.charAt(i++);
                        if (c == 'u') {
                            for (int digit = 0; digit < 4; ++digit) {
                                if (i == text.length()) {
                                    throw new IOException("Incomplete Unicode escape");
                                }
                                char hex = text.charAt(i++);
                                if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') || (hex >= 'A' && hex <= 'F'))) {
                                    throw new IOException("Invalid Unicode escape");
                                }
                            }
                        } else if (c != '"' && c != '\\' && c != '/' && c != 'b' && c != 'f' && c != 'n' && c != 'r' && c != 't') {
                            throw new IOException("Invalid JSON escape");
                        }
                    }
                }
                if (!closed) {
                    throw new IOException("Incomplete JSON string");
                }
            } else {
                int begin = i - 1;
                while (i < text.length()) {
                    c = text.charAt(i);
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}'
                            || c == '[' || c == ']' || c == ':' || c == ',' || c == '"') {
                        break;
                    }
                    if (i - begin >= 128) {
                        throw new IOException("JSON token limit exceeded");
                    }
                    ++i;
                }
                String literal = text.substring(begin, i);
                if (!literal.equals("true") && !literal.equals("false") && !literal.equals("null") && !NUMBER.matcher(literal).matches()) {
                    throw new IOException("Invalid JSON literal");
                }
            }
        }
    }

    private static String checkedString(String text) throws IOException {
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (Character.isHighSurrogate(c)) {
                if (++i == text.length() || !Character.isLowSurrogate(text.charAt(i))) {
                    throw new IOException("Invalid Unicode");
                }
            } else if (Character.isLowSurrogate(c)) {
                throw new IOException("Invalid Unicode");
            }
        }
        return text;
    }

    static String string(JSONObject object, String key) throws Exception {
        Object value = object.get(key);
        if (!(value instanceof String)) {
            throw new IOException("Expected string");
        }
        return (String) value;
    }

    static long integer(JSONObject object, String key) throws Exception {
        Object value = object.get(key);
        if (!(value instanceof Number)) {
            throw new IOException("Expected integer");
        }
        return new BigDecimal(value.toString()).longValueExact();
    }

    private StrictJson(AtomicBoolean cancelled) {
        this.cancelled = cancelled;
    }
}
