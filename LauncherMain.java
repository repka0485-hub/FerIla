package com.esp.launcher;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.InputStreamReader;

public class LauncherMain extends Activity {
    static final String GAME = "com.ScarletWork.CounterFlame";
    static final String DIR = "/data/local/tmp";
    TextView st;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        int p = (int) (24 * getResources().getDisplayMetrics().density);
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(p, p, p, p);
        TextView t = new TextView(this);
        t.setText("Counter Flame ESP");
        t.setTextSize(20);
        Button go = new Button(this);
        go.setText("ЗАПУСТИТЬ ИГРУ + ESP");
        st = new TextView(this);
        st.setText("готов");
        l.addView(t);
        l.addView(go);
        l.addView(st);
        setContentView(l);
        go.setOnClickListener(v -> start());
    }

    static String su(String cmd) {
        try {
            Process pr = Runtime.getRuntime().exec(new String[] {"su", "-c", cmd});
            BufferedReader r = new BufferedReader(new InputStreamReader(pr.getInputStream()));
            StringBuilder sb = new StringBuilder();
            String s;
            while ((s = r.readLine()) != null) sb.append(s).append('\n');
            pr.waitFor();
            return sb.toString().trim();
        } catch (Exception e) {
            return "ERR " + e.getMessage();
        }
    }

    void setStatus(final String s) { runOnUiThread(() -> st.setText(s)); }

    void start() {
        final Intent game = getPackageManager().getLaunchIntentForPackage(GAME);
        if (game == null) { st.setText("игра не установлена: " + GAME); return; }
        st.setText("жду игру, инжект сам...");
        new Thread(() -> {
            su("rm -f " + DIR + "/esp.log; nohup sh " + DIR + "/esp.sh inject >" + DIR + "/esp.log 2>&1 </dev/null &");
            for (int i = 0; i < 90; i++) {
                try { Thread.sleep(2000); } catch (InterruptedException e) { return; }
                String log = su("tail -n 2 " + DIR + "/esp.log");
                if (log.length() > 0) setStatus(log);
                if (log.contains("loaded") || log.contains("FAILED") || log.contains("not started")) return;
            }
        }).start();
        startActivity(game);
    }
}
