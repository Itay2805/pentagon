namespace System;

public class InvalidCastException : SystemException
{
    
    public InvalidCastException()
        : base("Specified cast is not valid.")
    {
    }

    public InvalidCastException(string message)
        : base(message)
    {
    }

    public InvalidCastException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
    
}