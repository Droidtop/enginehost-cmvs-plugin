package dev.enginehost.plugin.cmvs;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.MotionEvent;
import android.view.View;
import dev.enginehost.api.EngineControllerEvent;
import dev.enginehost.api.EnginePlugin;
import dev.enginehost.api.EnginePluginSession;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.Charset;
import java.util.Arrays;
import java.util.Locale;
import java.util.List;
import org.json.JSONObject;

/** Experimental in-process PS2/PS3 dialogue interpreter. */
public final class CmvsPlugin implements EnginePlugin {
    private EnginePluginSession session;
    private DialogueView view;

    @Override public void onCreate(EnginePluginSession session) throws Exception {
        this.session = session;
        if (!"cmvs".equals(session.engine()) || !("ps2".equals(session.engineContext()) || "ps3".equals(session.engineContext()))) {
            throw new IOException("Unsupported CMVS context");
        }
        view = new DialogueView(readScript());
        session.display().addView(view, new android.view.ViewGroup.LayoutParams(-1, -1));
    }

    /**
     * A real CMVS game keeps its scripts inside a CPZ archive, so that is where
     * this looks first; a loose extracted script is still accepted, because
     * that is how the reader was originally developed.
     */
    private List<String> readScript() throws Exception {
        File root = new File(session.gamePath()).getCanonicalFile();
        if (!root.isDirectory()) throw new IOException("CMVS game folder is unreadable");
        String context = session.engineContext();
        String exec = session.execFile();
        Charset encoding = resolveEncoding();

        if (exec != null && !exec.isBlank() && new File(root, exec).isFile()) {
            return CmvsScript.read(confined(root, exec, context), encoding);
        }
        File[] extracted = root.listFiles((dir, name) -> name.toLowerCase(Locale.ROOT).endsWith("." + context));
        if ((exec == null || exec.isBlank()) && extracted != null && extracted.length > 0) {
            Arrays.sort(extracted);
            return CmvsScript.read(extracted[0].getCanonicalFile(), encoding);
        }

        File archive = scriptArchive(root);
        try (CmvsArchive cpz = CmvsArchive.open(archive)) {
            session.host().log(android.util.Log.INFO, "cmvs", archive.getName() + ": "
                + cpz.names().size() + " entries, scheme " + cpz.schemeName(), null);
            CmvsArchive.Entry entry = chooseScript(cpz, exec, context);
            return CmvsScript.read(cpz.read(entry), false, entry.name, encoding);
        }
    }

    /** The archive holding the scripts, under the data folder cmvs.cfg names. */
    private File scriptArchive(File root) throws IOException {
        File folder = new File(root, scriptFolder(root));
        File archive = new File(folder, "script.cpz");
        if (!archive.isFile()) {
            throw new IOException("No CMVS script archive at " + folder.getName() + "/script.cpz");
        }
        return archive;
    }

    /** SCRIPT_INIT_PATH from cmvs.cfg, which every CMVS game ships; data/pack otherwise. */
    private String scriptFolder(File root) {
        File config = new File(root, "cmvs.cfg");
        if (config.isFile() && config.length() < 1024 * 1024) {
            try {
                for (String line : java.nio.file.Files.readAllLines(config.toPath(), Charset.forName("Shift_JIS"))) {
                    String trimmed = line.trim();
                    if (trimmed.toUpperCase(Locale.ROOT).startsWith("SCRIPT_INIT_PATH=")) {
                        String value = trimmed.substring("SCRIPT_INIT_PATH=".length()).trim().replace('\\', '/');
                        while (value.endsWith("/")) value = value.substring(0, value.length() - 1);
                        if (!value.isEmpty() && !value.startsWith("/") && !value.contains("..")) return value;
                    }
                }
            } catch (Exception unreadable) {
                session.host().log(android.util.Log.WARN, "cmvs", "Ignoring unreadable cmvs.cfg", unreadable);
            }
        }
        return "data/pack";
    }

    /** execFile may name an entry inside the archive; otherwise the game's own main script. */
    private CmvsArchive.Entry chooseScript(CmvsArchive cpz, String exec, String context) throws IOException {
        if (exec != null && !exec.isBlank()) {
            CmvsArchive.Entry named = cpz.find(exec);
            if (named == null) throw new IOException("No script named " + exec + " in the CMVS archive");
            return named;
        }
        CmvsArchive.Entry main = cpz.find("code/main." + context);
        if (main != null) return main;
        for (String name : cpz.names()) {
            if (name.endsWith("." + context)) return cpz.find(name);
        }
        throw new IOException("The CMVS script archive holds no ." + context + " script");
    }

    private File confined(File root, String relative, String context) throws IOException {
        if (new File(relative).isAbsolute()) throw new IOException("execFile must be relative");
        File file = new File(root, relative).getCanonicalFile();
        if (!file.isFile() || !file.getPath().startsWith(root.getPath() + File.separator) ||
            !file.getName().toLowerCase(java.util.Locale.ROOT).endsWith("." + context)) {
            throw new IOException("CMVS execFile leaves the folder or mismatches context");
        }
        return file;
    }

    private Charset resolveEncoding() throws Exception {
        String name = new JSONObject(session.optionsJson() == null ? "{}" : session.optionsJson())
            .optString("textEncoding", "Shift_JIS");
        if (!Charset.isSupported(name)) throw new IOException("Unsupported CMVS textEncoding: " + name);
        return Charset.forName(name);
    }

    @Override public boolean onControllerEvent(EngineControllerEvent event) {
        if (event.pressed() && ("confirm".equals(event.action()) || "page_next".equals(event.action()))) {
            view.advance(); return true;
        }
        return false;
    }

    private final class DialogueView extends View {
        private final List<String> strings;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int cursor;
        DialogueView(List<String> strings) { super(session.host().context()); this.strings = strings; paint.setColor(Color.WHITE); paint.setTextSize(34); setBackgroundColor(Color.BLACK); restore(); }
        void advance() { if (cursor + 1 < strings.size()) { cursor++; save(); invalidate(); } }
        private File stateFile() { return new File(session.host().saveDirectory(), "cmvs-experimental-state.json"); }
        private void restore() { try { File f=stateFile(); if(!f.isFile()||f.length()>1024*1024)return; byte[] b=new byte[(int)f.length()]; try(java.io.FileInputStream in=new java.io.FileInputStream(f)){int o=0;for(int n;o<b.length&&(n=in.read(b,o,b.length-o))>0;)o+=n;} cursor=Math.max(0,Math.min(Math.max(0,strings.size()-1),new JSONObject(new String(b,java.nio.charset.StandardCharsets.UTF_8)).optInt("cursor",0))); } catch(Exception e){session.host().log(android.util.Log.WARN,"cmvs","Ignoring invalid save",e);} }
        private void save() { try(FileOutputStream out=new FileOutputStream(stateFile(),false)){out.write(new JSONObject().put("cursor",cursor).toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));}catch(Exception e){session.host().log(android.util.Log.ERROR,"cmvs","Could not save",e);} }
        @Override protected void onDraw(Canvas canvas) { super.onDraw(canvas); String value=strings.isEmpty()?"No dialogue references found":strings.get(Math.min(cursor,strings.size()-1)); float y=getHeight()*.65f; for(int start=0;start<value.length();){int count=paint.breakText(value,start,value.length(),true,getWidth()-96,null);if(count<=0)break;canvas.drawText(value,start,start+count,48,y,paint);start+=count;y+=44;} }
        @Override public boolean onTouchEvent(MotionEvent event) { if(event.getAction()==MotionEvent.ACTION_UP)advance(); return true; }
    }
}
