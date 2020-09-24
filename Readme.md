# Curl Wrapper
A wrapper for curl multi interface with multi thread.

## Attention
- Use `signal(SIGPIPE, SIG_IGN);` to avoid the impact of SIGPIPE
- There may be memory leak under certain circumstances, [see this](https://bugzilla.redhat.com/show_bug.cgi?id=1688958)
