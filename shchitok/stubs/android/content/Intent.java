package android.content;
import android.net.Uri;
public class Intent {
  public Intent(String action){}
  public Intent(String action, Uri uri){}
  public Intent setDataAndType(Uri u, String t){return this;}
  public Intent addFlags(int f){return this;}
  public void setClipData(ClipData c){}
}
