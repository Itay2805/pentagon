using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace System.Threading;

public abstract class WaitHandle : IDisposable
{

    internal ulong Waitable = 0;

    // don't allow anyone but ourselves to inherit this
    internal WaitHandle()
    {
    }

    internal void Create(int count)
    {
        Waitable = CreateWaitable(count);
    }

    ~WaitHandle()
    {
        Dispose(false);
    }
    
    public virtual void Close()
    {
        Dispose();
    }
    
    public void Dispose()
    {
        Dispose(true);
        GC.SuppressFinalize(this);
    }

    protected virtual void Dispose(bool disposing)
    {
        if (Waitable == 0)
        {
            return;
        }
        
        ReleaseWaitable(Waitable);
        Waitable = 0;
    }

    #region Wait All

    public static bool WaitAll(WaitHandle[] waitHandles)
    {
        if (waitHandles == null)
            throw new ArgumentNullException(nameof(waitHandles));

        // TODO: verify no duplicates
        // TODO: verify no nulls
        
        foreach (var handle in waitHandles)
        {
            handle.WaitOne();

            // edge case, we need to make sure that manual reset
            // even will keep being set...
            if (handle is EventWaitHandle { _mode: EventResetMode.ManualReset } eventWaitHandle)
            {
                eventWaitHandle.Set();
            }
        }

        return true;
    }
    
    // TODO: wait all timeout 

    #endregion
    
    #region Wait One
    
    public virtual bool WaitOne()
    {
        return WaitOneInternal();
    }

    public virtual bool WaitOne(int millisecondsTimeout)
    {
        if (millisecondsTimeout == -1)
            return WaitOne();

        if (millisecondsTimeout < 0)
            throw new ArgumentOutOfRangeException(nameof(millisecondsTimeout), "Number must be either non-negative and less than or equal to Int32.MaxValue or -1.");

        return WaitOne(new TimeSpan(millisecondsTimeout * TimeSpan.TicksPerMillisecond));
    }

    public virtual bool WaitOne(TimeSpan timeout)
    {
        return WaitOneInternal(timeout);
    }

    internal bool WaitOneInternal()
    {
        if (Waitable == 0) 
            throw new ObjectDisposedException();
        
        return WaitableWait(Waitable, true) == 2;
    }

    internal bool WaitOneInternal(TimeSpan timeout)
    {
        if (Waitable == 0) 
            throw new ObjectDisposedException();

        // create the timeout, we take the value in imcroseconds
        var timeoutWaitable = WaitableAfter(timeout.Ticks / TimeSpan.TicksPerMillisecond * 1000);
                
        // wait on both of them
        var selected = WaitableSelect2(Waitable, timeoutWaitable, true);

        // release the timeout variable
        ReleaseWaitable(timeoutWaitable);

        return selected == 0;
    }

    #endregion

    #region Native

    [MethodImpl(MethodImplOptions.InternalCall)]
    internal static extern bool WaitableSend(ulong waitable, bool block);

    [MethodImpl(MethodImplOptions.InternalCall)]
    internal static extern int WaitableWait(ulong waitable, bool block);

    [MethodImpl(MethodImplOptions.InternalCall)]
    private static extern int WaitableSelect2(ulong waitable1, ulong waitable2, bool block);

    [MethodImpl(MethodImplOptions.InternalCall)]
    private static extern ulong CreateWaitable(int count);

    [MethodImpl(MethodImplOptions.InternalCall)]
    private static extern ulong WaitableAfter(long timeout);

    [MethodImpl(MethodImplOptions.InternalCall)]
    private static extern void ReleaseWaitable(ulong waitable);

    #endregion

}