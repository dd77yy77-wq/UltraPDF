package android.content;
import android.net.Uri;
public class Intent {
  public Intent(String action){}
  public Intent(String action, Uri uri){}
  public Intent setDataAndType(Uri u, String t){return this;}
  public Intent addFlags(int f){return this;}
  public void setClipData(ClipData c){}
  public Intent setType(String t){return this;}
  public Intent putExtra(String k, android.os.Parcelable v){return this;}
  public Intent putExtra(String k, String v){return this;}
  public static Intent createChooser(Intent i, CharSequence t){return null;}
}
