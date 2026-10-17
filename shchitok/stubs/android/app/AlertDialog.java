package android.app;
import android.content.Context;
import android.content.DialogInterface;
public class AlertDialog {
  public void setMessage(CharSequence m){}
  public void dismiss(){}
  public static class Builder {
    public Builder(Context c){}
    public Builder setTitle(CharSequence t){return this;}
    public Builder setMessage(CharSequence m){return this;}
    public Builder setCancelable(boolean b){return this;}
    public Builder setPositiveButton(CharSequence t, DialogInterface.OnClickListener l){return this;}
    public Builder setNegativeButton(CharSequence t, DialogInterface.OnClickListener l){return this;}
    public AlertDialog show(){return null;}
  }
}
