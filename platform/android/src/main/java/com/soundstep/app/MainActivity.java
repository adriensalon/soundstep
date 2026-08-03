package com.soundstep.app;

import android.app.NativeActivity;
import android.content.Context;
import android.graphics.Color;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

import java.util.concurrent.ConcurrentLinkedQueue;

public final class MainActivity extends NativeActivity {
    public static final int INPUT_EVENT_BACKSPACE = -1;
    public static final int INPUT_EVENT_ENTER = -2;

    private final ConcurrentLinkedQueue<Integer> inputEvents = new ConcurrentLinkedQueue<>();
    private EditText inputProxy;
    private boolean clearingInputProxy;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        inputProxy = new EditText(this) {
            @Override
            public InputConnection onCreateInputConnection(EditorInfo editorInfo) {
                InputConnection connection = super.onCreateInputConnection(editorInfo);
                if (connection == null) {
                    return null;
                }
                return new InputConnectionWrapper(connection, false) {
                    @Override
                    public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                        queueBackspaces(beforeLength);
                        return true;
                    }

                    @Override
                    public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
                        queueBackspaces(beforeLength);
                        return true;
                    }

                    @Override
                    public boolean sendKeyEvent(KeyEvent event) {
                        if (event.getAction() == KeyEvent.ACTION_DOWN) {
                            if (event.getKeyCode() == KeyEvent.KEYCODE_DEL) {
                                inputEvents.offer(INPUT_EVENT_BACKSPACE);
                                return true;
                            }
                            if (event.getKeyCode() == KeyEvent.KEYCODE_ENTER) {
                                inputEvents.offer(INPUT_EVENT_ENTER);
                                return true;
                            }
                        }
                        return super.sendKeyEvent(event);
                    }
                };
            }
        };
        inputProxy.setSingleLine(true);
        inputProxy.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        inputProxy.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        inputProxy.setBackgroundColor(Color.TRANSPARENT);
        inputProxy.setTextColor(Color.TRANSPARENT);
        inputProxy.setCursorVisible(false);
        inputProxy.setAlpha(0.01f);
        inputProxy.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence text, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence text, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable text) {
                if (clearingInputProxy || text.length() == 0) {
                    return;
                }
                queueText(text);
                clearingInputProxy = true;
                text.clear();
                clearingInputProxy = false;
            }
        });
        inputProxy.setOnEditorActionListener((view, actionId, event) -> {
            inputEvents.offer(INPUT_EVENT_ENTER);
            return true;
        });

        FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(1, 1, Gravity.TOP | Gravity.START);
        addContentView(inputProxy, layout);
    }

    public void showSoftInput() {
        runOnUiThread(() -> {
            inputProxy.requestFocus();
            InputMethodManager manager = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            manager.restartInput(inputProxy);
            manager.showSoftInput(inputProxy, InputMethodManager.SHOW_IMPLICIT);
        });
    }

    public void hideSoftInput() {
        runOnUiThread(() -> {
            InputMethodManager manager = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            manager.hideSoftInputFromWindow(inputProxy.getWindowToken(), 0);
            inputProxy.clearFocus();
        });
    }

    public int pollInputEvent() {
        Integer event = inputEvents.poll();
        return event == null ? 0 : event;
    }

    private void queueBackspaces(int count) {
        for (int index = 0; index < Math.max(1, count); ++index) {
            inputEvents.offer(INPUT_EVENT_BACKSPACE);
        }
    }

    private void queueText(CharSequence text) {
        for (int offset = 0; offset < text.length();) {
            int codePoint = Character.codePointAt(text, offset);
            inputEvents.offer(codePoint);
            offset += Character.charCount(codePoint);
        }
    }
}
