package dev.enginehost.api;
import android.content.Context; import java.io.File;
public interface EngineHost { Context context(); File saveDirectory(); File cacheDirectory(); EngineFileSystem fileSystem(); void log(int p,String t,String m,Throwable e); boolean rumbleController(int id,long ms,int amplitude); void finish(); }
