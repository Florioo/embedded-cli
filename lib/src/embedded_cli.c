#include <stdlib.h>
#include <string.h>

#include "embedded_cli.h"

#define CLI_TOKEN_NPOS 0xffff

#define UNUSED(x) (void)x

#define PREPARE_IMPL(t) \
  EmbeddedCliImpl* impl = (EmbeddedCliImpl*)t->_impl

#define IS_FLAG_SET(flags, flag) (((flags) & (flag)) != 0)

#define SET_FLAG(flags, flag) ((flags) |= (flag))

#define UNSET_U8FLAG(flags, flag) ((flags) &= (uint8_t) ~(flag))

/**
 * Marks binding as candidate for autocompletion
 * This flag is updated each time getAutocompletedCommand is called
 */
#define BINDING_FLAG_AUTOCOMPLETE 1u

/**
 * Indicates that rx buffer overflow happened. In such case last command
 * that wasn't finished (no \r or \n were received) will be discarded
 */
#define CLI_FLAG_OVERFLOW 0x01u

/**
 * Indicates that initialization is completed. Initialization is completed in
 * first call to process and needed, for example, to print invitation message.
 */
#define CLI_FLAG_INIT_COMPLETE 0x02u

/**
 * Indicates that CLI structure and internal structures were allocated with
 * malloc and should bre freed
 */
#define CLI_FLAG_ALLOCATED 0x04u

/**
 * Indicates that CLI structure and internal structures were allocated with
 * malloc and should bre freed
 */
#define CLI_FLAG_ESCAPE_MODE 0x08u

/**
 * Indicates that CLI in mode when it will print directly to output without
 * clear of current command and printing it back
 */
#define CLI_FLAG_DIRECT_PRINT 0x10u

/**
 * Indicates that live autocompletion is enabled
 */
#define CLI_FLAG_AUTOCOMPLETE_ENABLED 0x20u

typedef struct EmbeddedCliImpl EmbeddedCliImpl;
typedef struct AutocompletedCommand AutocompletedCommand;
typedef struct FifoBuf FifoBuf;
typedef struct CliHistory CliHistory;

struct FifoBuf {
    char *buf;
    /**
     * Position of first element in buffer. From this position elements are taken
     */
    uint16_t front;
    /**
     * Position after last element. At this position new elements are inserted
     */
    uint16_t back;
    /**
     * Size of buffer
     */
    uint16_t size;
};

struct CliHistory {
    /**
     * Items in buffer are separated by null-chars
     */
    char *buf;

    /**
     * Total size of buffer
     */
    uint16_t bufferSize;

    /**
     * Index of currently selected element. This allows to navigate history
     * After command is sent, current element is reset to 0 (no element)
     */
    uint16_t current;

    /**
     * Number of items in buffer
     * Items are counted from top to bottom (and are 1 based).
     * So the most recent item is 1 and the oldest is itemCount.
     */
    uint16_t itemsCount;
};

struct EmbeddedCliImpl {
    /**
     * Invitation string. Is printed at the beginning of each line with user
     * input
     */
    const char *invitation;

    CliHistory history;

    /**
     * Buffer for storing received chars.
     * Chars are stored in FIFO mode.
     */
    FifoBuf rxBuffer;

    /**
     * Buffer for current command
     */
    char *cmdBuffer;

    /**
     * Size of current command
     */
    uint16_t cmdSize;

    /**
     * Total size of command buffer
     */
    uint16_t cmdMaxSize;

    CliCommandBinding *bindings;

    /**
     * Flags for each binding. Sizes are the same as for bindings array
     */
    uint8_t *bindingsFlags;

    uint16_t bindingsCount;

    uint16_t maxBindingsCount;

    /**
     * Total length of input line. This doesn't include invitation but
     * includes current command and its live autocompletion
     */
    uint16_t inputLineLength;

    /**
     * Stores last character that was processed.
     */
    char lastChar;

    /**
     * Flags are defined as CLI_FLAG_*
     */
    uint8_t flags;
};

struct AutocompletedCommand {
    /**
     * Name of autocompleted command (or first candidate for autocompletion if
     * there are multiple candidates).
     * NULL if autocomplete not possible.
     */
    const char *firstCandidate;

    /**
     * Number of characters that can be completed safely. For example, if there
     * are two possible commands "get-led" and "get-adc", then for prefix "g"
     * autocompletedLen will be 4. If there are only one candidate, this number
     * is always equal to length of the command.
     */
    uint16_t autocompletedLen;

    /**
     * Total number of candidates for autocompletion
     */
    uint16_t candidateCount;
};

static EmbeddedCliConfig defaultConfig;

/**
 * Number of commands that cli adds. Commands:
 * - help
 */
static const uint16_t cliInternalBindingCount = 1;

static const char *lineBreak = "\r\n";

/**
 * Navigate through command history back and forth. If navigateUp is true,
 * navigate to older commands, otherwise navigate to newer.
 * When history end is reached, nothing happens.
 * @param cli
 * @param navigateUp
 */
static void navigateHistory(EmbeddedCli *cli, bool navigateUp);

/**
 * Process escaped character. After receiving ESC+[ sequence, all chars up to
 * ending character are sent to this function
 * @param cli
 * @param c
 */
static void onEscapedInput(EmbeddedCli *cli, char c);

/**
 * Process input character. Character is valid displayable char and should be
 * added to current command string and displayed to client.
 * @param cli
 * @param c
 */
static void onCharInput(EmbeddedCli *cli, char c);

/**
 * Process control character (like \r or \n) possibly altering state of current
 * command or executing onCommand callback.
 * @param cli
 * @param c
 */
static void onControlInput(EmbeddedCli *cli, char c, void *handle);

/**
 * Parse command in buffer and execute callback
 * @param cli
 */
static void parseCommand(EmbeddedCli *cli, void * handle);

/**
 * Setup bindings for internal commands, like help
 * @param cli
 */
static void initInternalBindings(EmbeddedCli *cli);

/**
 * Show help for given tokens (or default help if no tokens)
 * @param cli
 * @param tokens
 * @param context - not used
 */
static int onHelp(EmbeddedCli *cli, char *tokens, void *context);

/**
 * Show error about unknown command
 * @param cli
 * @param name
 */
static void onUnknownCommand(EmbeddedCli *cli, const char *name);

/**
 * Return autocompleted command for given prefix.
 * Prefix is compared to all known command bindings and autocompleted result
 * is returned
 * @param cli
 * @param prefix
 * @return
 */
static AutocompletedCommand getAutocompletedCommand(EmbeddedCli *cli, const char *prefix);

/**
 * Prints autocompletion result while keeping current command unchanged
 * Prints only if autocompletion is present and only one candidate exists.
 * @param cli
 */
static void printLiveAutocompletion(EmbeddedCli *cli);

/**
 * Handles autocomplete request. If autocomplete possible - fills current
 * command with autocompleted command. When multiple commands satisfy entered
 * prefix, they are printed to output.
 * @param cli
 */
static void onAutocompleteRequest(EmbeddedCli *cli);

/**
 * Removes all input from current line (replaces it with whitespaces)
 * And places cursor at the beginning of the line
 * @param cli
 */
static void clearCurrentLine(EmbeddedCli *cli);

/**
 * Write given string to cli output
 * @param cli
 * @param str
 */
static void writeToOutput(EmbeddedCli *cli, const char *str);

/**
 * Returns true if provided char is a supported control char:
 * \r, \n, \b or 0x7F (treated as \b)
 * @param c
 * @return
 */
static bool isControlChar(char c);

/**
 * Returns true if provided char is a valid displayable character:
 * a-z, A-Z, 0-9, whitespace, punctuation, etc.
 * Currently only ASCII is supported
 * @param c
 * @return
 */
static bool isDisplayableChar(char c);

/**
 * How many elements are currently available in buffer
 * @param buffer
 * @return number of elements
 */
static uint16_t fifoBufAvailable(FifoBuf *buffer);

/**
 * Return first character from buffer and remove it from buffer
 * Buffer must be non-empty, otherwise 0 is returned
 * @param buffer
 * @return
 */
static char fifoBufPop(FifoBuf *buffer);

/**
 * Push character into fifo buffer. If there is no space left, character is
 * discarded and false is returned
 * @param buffer
 * @param a - character to add
 * @return true if char was added to buffer, false otherwise
 */
static bool fifoBufPush(FifoBuf *buffer, char a);

/**
 * Copy provided string to the history buffer.
 * If it is already inside history, it will be removed from it and added again.
 * So after addition, it will always be on top
 * If available size is not enough (and total size is enough) old elements will
 * be removed from history so this item can be put to it
 * @param history
 * @param str
 * @return true if string was put in history
 */
static bool historyPut(CliHistory *history, const char *str);

/**
 * Get item from history. Items are counted from 1 so if item is 0 or greater
 * than itemCount, NULL is returned
 * @param history
 * @param item
 * @return true if string was put in history
 */
static const char *historyGet(CliHistory *history, uint16_t item);

/**
 * Remove specific item from history
 * @param history
 * @param str - string to remove
 * @return
 */
static void historyRemove(CliHistory *history, const char *str);

/**
 * Return position (index of first char) of specified token
 * @param tokenizedStr - tokenized string (separated by \0 with
 * \0\0 at the end)
 * @param pos - token position (counted from 1)
 * @return index of first char of specified token
 */
static uint16_t getTokenPosition(const char *tokenizedStr, uint16_t pos);

EmbeddedCliConfig *embeddedCliDefaultConfig(void) {
    defaultConfig.rxBufferSize = 64;
    defaultConfig.cmdBufferSize = 64;
    defaultConfig.historyBufferSize = 128;
    defaultConfig.cliBuffer = NULL;
    defaultConfig.cliBufferSize = 0;
    defaultConfig.maxBindingCount = 8;
    defaultConfig.enableAutoComplete = true;
    defaultConfig.invitation = "> ";
    return &defaultConfig;
}

uint16_t embeddedCliRequiredSize(EmbeddedCliConfig *config) {
    uint16_t bindingCount = (uint16_t) (config->maxBindingCount + cliInternalBindingCount);
    return (uint16_t) (CLI_UINT_SIZE * (
            BYTES_TO_CLI_UINTS(sizeof(EmbeddedCli)) +
            BYTES_TO_CLI_UINTS(sizeof(EmbeddedCliImpl)) +
            BYTES_TO_CLI_UINTS(config->rxBufferSize * sizeof(char)) +
            BYTES_TO_CLI_UINTS(config->cmdBufferSize * sizeof(char)) +
            BYTES_TO_CLI_UINTS(config->historyBufferSize * sizeof(char)) +
            BYTES_TO_CLI_UINTS(bindingCount * sizeof(CliCommandBinding)) +
            BYTES_TO_CLI_UINTS(bindingCount * sizeof(uint8_t))));
}

EmbeddedCli *embeddedCliNew(EmbeddedCliConfig *config) {
    EmbeddedCli *cli = NULL;

    uint16_t bindingCount = (uint16_t) (config->maxBindingCount + cliInternalBindingCount);

    size_t totalSize = embeddedCliRequiredSize(config);

    bool allocated = false;
    if (config->cliBuffer == NULL) {
        config->cliBuffer = (CLI_UINT *) malloc(totalSize); // malloc guarantees alignment.
        if (config->cliBuffer == NULL)
            return NULL;
        allocated = true;
    } else if (config->cliBufferSize < totalSize) {
        return NULL;
    }

    CLI_UINT *buf = config->cliBuffer;

    memset(buf, 0, totalSize);

    cli = (EmbeddedCli *) buf;
    buf += BYTES_TO_CLI_UINTS(sizeof(EmbeddedCli));

    cli->_impl = (EmbeddedCliImpl *) buf;
    buf += BYTES_TO_CLI_UINTS(sizeof(EmbeddedCliImpl));

    PREPARE_IMPL(cli);
    impl->rxBuffer.buf = (char *) buf;
    buf += BYTES_TO_CLI_UINTS(config->rxBufferSize * sizeof(char));

    impl->cmdBuffer = (char *) buf;
    buf += BYTES_TO_CLI_UINTS(config->cmdBufferSize * sizeof(char));

    impl->bindings = (CliCommandBinding *) buf;
    buf += BYTES_TO_CLI_UINTS(bindingCount * sizeof(CliCommandBinding));

    impl->bindingsFlags = (uint8_t *) buf;
    buf += BYTES_TO_CLI_UINTS(bindingCount);

    impl->history.buf = (char *) buf;
    impl->history.bufferSize = config->historyBufferSize;

    if (allocated)
        SET_FLAG(impl->flags, CLI_FLAG_ALLOCATED);

    if (config->enableAutoComplete)
        SET_FLAG(impl->flags, CLI_FLAG_AUTOCOMPLETE_ENABLED);

    impl->rxBuffer.size = config->rxBufferSize;
    impl->rxBuffer.front = 0;
    impl->rxBuffer.back = 0;
    impl->cmdMaxSize = config->cmdBufferSize;
    impl->bindingsCount = 0;
    impl->maxBindingsCount = (uint16_t) (config->maxBindingCount + cliInternalBindingCount);
    impl->lastChar = '\0';
    impl->invitation = config->invitation;
    initInternalBindings(cli);

    return cli;
}

EmbeddedCli *embeddedCliNewDefault(void) {
    return embeddedCliNew(embeddedCliDefaultConfig());
}

void embeddedCliReceiveChar(EmbeddedCli *cli, char c) {
    PREPARE_IMPL(cli);

    if (!fifoBufPush(&impl->rxBuffer, c)) {
        SET_FLAG(impl->flags, CLI_FLAG_OVERFLOW);
    }
}

void embeddedCliProcess(EmbeddedCli *cli, void * handle) {
    if (cli->writeChar == NULL)
        return;

    PREPARE_IMPL(cli);


    if (!IS_FLAG_SET(impl->flags, CLI_FLAG_INIT_COMPLETE)) {
        SET_FLAG(impl->flags, CLI_FLAG_INIT_COMPLETE);
        writeToOutput(cli, impl->invitation);
    }

    while (fifoBufAvailable(&impl->rxBuffer)) {
        char c = fifoBufPop(&impl->rxBuffer);

        if (IS_FLAG_SET(impl->flags, CLI_FLAG_ESCAPE_MODE)) {
            onEscapedInput(cli, c);
        } else if (impl->lastChar == 0x1B && c == '[') {
            //enter escape mode
            SET_FLAG(impl->flags, CLI_FLAG_ESCAPE_MODE);
        } else if (isControlChar(c)) {
            onControlInput(cli, c, handle);
        } else if (isDisplayableChar(c)) {
            onCharInput(cli, c);
        }

        printLiveAutocompletion(cli);

        impl->lastChar = c;
    }

    // discard unfinished command if overflow happened
    if (IS_FLAG_SET(impl->flags, CLI_FLAG_OVERFLOW)) {
        impl->cmdSize = 0;
        impl->cmdBuffer[impl->cmdSize] = '\0';
        UNSET_U8FLAG(impl->flags, CLI_FLAG_OVERFLOW);
    }
}

bool embeddedCliAddBinding(EmbeddedCli *cli, CliCommandBinding binding) {
    PREPARE_IMPL(cli);
    if (impl->bindingsCount == impl->maxBindingsCount)
        return false;

    impl->bindings[impl->bindingsCount] = binding;

    ++impl->bindingsCount;
    return true;
}

void embeddedCliPrint(EmbeddedCli *cli, const char *string) {
    if (cli->writeChar == NULL)
        return;

    PREPARE_IMPL(cli);

    // remove chars for autocompletion and live command
    if (!IS_FLAG_SET(impl->flags, CLI_FLAG_DIRECT_PRINT))
        clearCurrentLine(cli);

    // print provided string
    writeToOutput(cli, string);
    writeToOutput(cli, lineBreak);

    // print current command back to screen
    if (!IS_FLAG_SET(impl->flags, CLI_FLAG_DIRECT_PRINT)) {
        writeToOutput(cli, impl->invitation);
        writeToOutput(cli, impl->cmdBuffer);
        impl->inputLineLength = impl->cmdSize;

        printLiveAutocompletion(cli);
    }
}

void embeddedCliFree(EmbeddedCli *cli) {
    PREPARE_IMPL(cli);
    if (IS_FLAG_SET(impl->flags, CLI_FLAG_ALLOCATED)) {
        // allocation is done in single call to malloc, so need only single free
        free(cli);
    }
}

void embeddedCliTokenizeArgs(char *args) {
    if (args == NULL)
        return;

    // for now only space, but can add more later
    const char *separators = " ";

    // indicates that arg is quoted so separators are copied as is
    bool quotesEnabled = false;
    // indicates that previous char was a slash, so next char is copied as is
    bool escapeActivated = false;
    int insertPos = 0;

    int i = 0;
    char currentChar;
    while ((currentChar = args[i]) != '\0') {
        ++i;

        if (escapeActivated) {
            escapeActivated = false;
        } else if (currentChar == '\\') {
            escapeActivated = true;
            continue;
        } else if (currentChar == '"') {
            quotesEnabled = !quotesEnabled;
            currentChar = '\0';
        } else if (!quotesEnabled && strchr(separators, currentChar) != NULL) {
            currentChar = '\0';
        }

        // null chars are only copied once and not copied to the beginning
        if (currentChar != '\0' || (insertPos > 0 && args[insertPos - 1] != '\0')) {
            args[insertPos] = currentChar;
            ++insertPos;
        }
    }

    // make args double null-terminated source buffer must be big enough to contain extra spaces
    args[insertPos] = '\0';
    args[insertPos + 1] = '\0';
}

const char *embeddedCliGetToken(const char *tokenizedStr, uint16_t pos) {
    uint16_t i = getTokenPosition(tokenizedStr, pos);

    if (i != CLI_TOKEN_NPOS)
        return &tokenizedStr[i];
    else
        return NULL;
}

char *embeddedCliGetTokenVariable(char *tokenizedStr, uint16_t pos) {
    uint16_t i = getTokenPosition(tokenizedStr, pos);

    if (i != CLI_TOKEN_NPOS)
        return &tokenizedStr[i];
    else
        return NULL;
}

uint16_t embeddedCliFindToken(const char *tokenizedStr, const char *token) {
    if (tokenizedStr == NULL || token == NULL)
        return 0;

    uint16_t size = embeddedCliGetTokenCount(tokenizedStr);
    for (uint16_t i = 1; i <= size; ++i) {
        if (strcmp(embeddedCliGetToken(tokenizedStr, i), token) == 0)
            return i;
    }

    return 0;
}

uint16_t embeddedCliGetTokenCount(const char *tokenizedStr) {
    if (tokenizedStr == NULL || tokenizedStr[0] == '\0')
        return 0;

    int i = 0;
    uint16_t tokenCount = 1;
    while (true) {
        if (tokenizedStr[i] == '\0') {
            if (tokenizedStr[i + 1] == '\0')
                break;
            ++tokenCount;
        }
        ++i;
    }

    return tokenCount;
}

static void navigateHistory(EmbeddedCli *cli, bool navigateUp) {
    PREPARE_IMPL(cli);
    if (impl->history.itemsCount == 0 ||
        (navigateUp && impl->history.current == impl->history.itemsCount) ||
        (!navigateUp && impl->history.current == 0))
        return;

    clearCurrentLine(cli);

    writeToOutput(cli, impl->invitation);

    if (navigateUp)
        ++impl->history.current;
    else
        --impl->history.current;

    const char *item = historyGet(&impl->history, impl->history.current);
    // simple way to handle empty command the same way as others
    if (item == NULL)
        item = "";
    uint16_t len = (uint16_t) strlen(item);
    memcpy(impl->cmdBuffer, item, len);
    impl->cmdBuffer[len] = '\0';
    impl->cmdSize = len;

    writeToOutput(cli, impl->cmdBuffer);
    impl->inputLineLength = impl->cmdSize;

    printLiveAutocompletion(cli);
}

static void onEscapedInput(EmbeddedCli *cli, char c) {
    PREPARE_IMPL(cli);

    if (c >= 64 && c <= 126) {
        // handle escape sequence
        UNSET_U8FLAG(impl->flags, CLI_FLAG_ESCAPE_MODE);

        if (c == 'A' || c == 'B') {
            // treat \e[..A as cursor up and \e[..B as cursor down
            // there might be extra chars between [ and A/B, just ignore them
            navigateHistory(cli, c == 'A');
        }
    }
}

static void onCharInput(EmbeddedCli *cli, char c) {
    PREPARE_IMPL(cli);

    // have to reserve two extra chars for command ending (used in tokenization)
    if (impl->cmdSize + 2 >= impl->cmdMaxSize)
        return;

    impl->cmdBuffer[impl->cmdSize] = c;
    ++impl->cmdSize;
    impl->cmdBuffer[impl->cmdSize] = '\0';

    cli->writeChar(cli, c);
}

static void onControlInput(EmbeddedCli *cli, char c, void *handle) {
    PREPARE_IMPL(cli);
    
    // process \r\n and \n\r as single \r\n command
    if ((impl->lastChar == '\r' && c == '\n') ||
        (impl->lastChar == '\n' && c == '\r'))
        return;

    if (c == '\r' || c == '\n') {
        // try to autocomplete command and then process it
        onAutocompleteRequest(cli);

        writeToOutput(cli, lineBreak);

        if (impl->cmdSize > 0)
            parseCommand(cli, handle);
        impl->cmdSize = 0;
        impl->cmdBuffer[impl->cmdSize] = '\0';
        impl->inputLineLength = 0;
        impl->history.current = 0;

        writeToOutput(cli, impl->invitation);
    } else if ((c == '\b' || c == 0x7F) && impl->cmdSize > 0) {
        // remove char from screen
        cli->writeChar(cli, '\b');
        cli->writeChar(cli, ' ');
        cli->writeChar(cli, '\b');
        // and from buffer
        --impl->cmdSize;
        impl->cmdBuffer[impl->cmdSize] = '\0';
    } else if (c == '\t') {
        onAutocompleteRequest(cli);
    }

}
#include <stdio.h>

static int _parseCommand(EmbeddedCli *cli,  char * command_buffer, uint16_t command_size, bool direct_mode, void * handle)
{
    PREPARE_IMPL(cli);

    bool isEmpty = true;

    for (int i = 0; i < command_size; ++i) {
        if (command_buffer[i] != ' ') {
            isEmpty = false;
            break;
        }
    }
    // do not process empty commands
    if (isEmpty)
        return 1;
    
    if (!direct_mode){
        // push command to history before buffer is modified
        historyPut(&impl->history, command_buffer);
    }

    char *cmdName = NULL;
    char *cmdArgs = NULL;
    bool nameFinished = false;

    // find command name and command args inside command buffer
    for (int i = 0; i < command_size; ++i) {
        char c = command_buffer[i];

        if (c == ' ') {
            // all spaces between name and args are filled with zeros
            // so name is a correct null-terminated string
            if (cmdArgs == NULL)
                command_buffer[i] = '\0';
            if (cmdName != NULL)
                nameFinished = true;

        } else if (cmdName == NULL) {
            cmdName = &command_buffer[i];
        } else if (cmdArgs == NULL && nameFinished) {
            cmdArgs = &command_buffer[i];
        }
    }
    // we keep two last bytes in cmd buffer reserved so cmdSize is always by 2
    // less than cmdMaxSize
    command_buffer[command_size + 1] = '\0';

    if (cmdName == NULL)
        return 1;

    // try to find command in bindings
    for (int i = 0; i < impl->bindingsCount; ++i) {
        if (strcmp(cmdName, impl->bindings[i].name) == 0) {
            if (impl->bindings[i].binding == NULL)
                break;

            if (impl->bindings[i].tokenizeArgs)
                embeddedCliTokenizeArgs(cmdArgs);

            // currently, output is blank line, so we can just print directly
            if (!direct_mode){
                SET_FLAG(impl->flags, CLI_FLAG_DIRECT_PRINT);
            }

            uint8_t result = impl->bindings[i].binding(handle, cmdArgs, impl->bindings[i].context);

            if (cli->postCommand)
                cli->postCommand(handle, result);

            if (!direct_mode){
                 UNSET_U8FLAG(impl->flags, CLI_FLAG_DIRECT_PRINT);
            }
            return 0;
        }
    }

    // TODO handle direct mode if there is no binding
    if (direct_mode){
        // Send error
        
        return 1;
    }

    // command not found in bindings or binding was null
    // try to call default callback
    if (cli->onCommand != NULL) {
        CliCommand command;
        command.name = cmdName;
        command.args = cmdArgs;

        // currently, output is blank line, so we can just print directly
        SET_FLAG(impl->flags, CLI_FLAG_DIRECT_PRINT);
        cli->onCommand(cli, &command);
        UNSET_U8FLAG(impl->flags, CLI_FLAG_DIRECT_PRINT);
    } else {
        onUnknownCommand(cli, cmdName);
        if (cli->postCommand)
            cli->postCommand(handle, 1);
            
    }
    return 1;
}

int embeddedCliParseDirectCommand(EmbeddedCli *cli, uint8_t *command,uint16_t length, void * handle) {
    
   
    //Copy command to buffer so it can be modified
    //Also add extra space for double null termination
   char * buffer = malloc(length + 2);
   strncpy(buffer, (char*) command, length);
    buffer[length] = '\0';
    // printf("Parsing direct command: '%s'\n", buffer);

   //Parse the command in direct mode
   int ret = _parseCommand(cli, buffer, length, true, handle);
   
   //Free the buffer
   free(buffer);
   return ret;
}


static void parseCommand(EmbeddedCli *cli, void * handle) {
    //Parse the command in REPL mode
    PREPARE_IMPL(cli);
    // printf("Parsing command: '%s'\n", impl->cmdBuffer);
    // printf("size: %d\n", impl->cmdSize);
    // printf("handle: %p\n", handle);
    // printf("cli: %p\n", cli);

    _parseCommand(cli, impl->cmdBuffer, impl->cmdSize, false, handle);
}


static void initInternalBindings(EmbeddedCli *cli) {
    CliCommandBinding b = {
            "help",
            "Print list of commands",
            true,
            NULL,
            (void*)onHelp
    };
    embeddedCliAddBinding(cli, b);
}

static int onHelp(EmbeddedCli *cli, char *tokens, void *context) {
    //Dirty hack to get the cli pointer 
    cli = *(EmbeddedCli **)cli;

    UNUSED(context);
    PREPARE_IMPL(cli);

    if (impl->bindingsCount == 0) {
        writeToOutput(cli, "Help is not available");
        writeToOutput(cli, lineBreak);
        return 1;
    }

    uint16_t tokenCount = embeddedCliGetTokenCount(tokens);
    if (tokenCount == 0) {
        for (int i = 0; i < impl->bindingsCount; ++i) {
            writeToOutput(cli, " * ");
            writeToOutput(cli, impl->bindings[i].name);
            writeToOutput(cli, lineBreak);
            if (impl->bindings[i].help != NULL) {
                cli->writeChar(cli, '\t');
                writeToOutput(cli, impl->bindings[i].help);
                writeToOutput(cli, lineBreak);
            }
        }
    } else if (tokenCount == 1) {
        // try find command
        const char *helpStr = NULL;
        const char *cmdName = embeddedCliGetToken(tokens, 1);
        bool found = false;
        for (int i = 0; i < impl->bindingsCount; ++i) {
            if (strcmp(impl->bindings[i].name, cmdName) == 0) {
                helpStr = impl->bindings[i].help;
                found = true;
                break;
            }
        }
        if (found && helpStr != NULL) {
            writeToOutput(cli, " * ");
            writeToOutput(cli, cmdName);
            writeToOutput(cli, lineBreak);
            cli->writeChar(cli, '\t');
            writeToOutput(cli, helpStr);
            writeToOutput(cli, lineBreak);
        } else if (found) {
            writeToOutput(cli, "Help is not available");
            writeToOutput(cli, lineBreak);
            return 1;

        } else {
            onUnknownCommand(cli, cmdName);
            return 1;

        }
    } else {
        writeToOutput(cli, "Command \"help\" receives one or zero arguments");
        writeToOutput(cli, lineBreak);
    }
    
    return 0;
}

static void onUnknownCommand(EmbeddedCli *cli, const char *name) {
    writeToOutput(cli, "Unknown command: \"");
    writeToOutput(cli, name);
    writeToOutput(cli, "\". Write \"help\" for a list of available commands");
    writeToOutput(cli, lineBreak);
}

static AutocompletedCommand getAutocompletedCommand(EmbeddedCli *cli, const char *prefix) {
    AutocompletedCommand cmd = {NULL, 0, 0};

    size_t prefixLen = strlen(prefix);

    PREPARE_IMPL(cli);
    if (impl->bindingsCount == 0 || prefixLen == 0)
        return cmd;


    for (int i = 0; i < impl->bindingsCount; ++i) {
        const char *name = impl->bindings[i].name;
        size_t len = strlen(name);

        // unset autocomplete flag
        UNSET_U8FLAG(impl->bindingsFlags[i], BINDING_FLAG_AUTOCOMPLETE);

        if (len < prefixLen)
            continue;

        // check if this command is candidate for autocomplete
        bool isCandidate = true;
        for (size_t j = 0; j < prefixLen; ++j) {
            if (prefix[j] != name[j]) {
                isCandidate = false;
                break;
            }
        }
        if (!isCandidate)
            continue;

        impl->bindingsFlags[i] |= BINDING_FLAG_AUTOCOMPLETE;

        if (cmd.candidateCount == 0 || len < cmd.autocompletedLen)
            cmd.autocompletedLen = (uint16_t) len;

        ++cmd.candidateCount;

        if (cmd.candidateCount == 1) {
            cmd.firstCandidate = name;
            continue;
        }

        for (size_t j = impl->cmdSize; j < cmd.autocompletedLen; ++j) {
            if (cmd.firstCandidate[j] != name[j]) {
                cmd.autocompletedLen = (uint16_t) j;
                break;
            }
        }
    }

    return cmd;
}

static void printLiveAutocompletion(EmbeddedCli *cli) {
    PREPARE_IMPL(cli);

    if (!IS_FLAG_SET(impl->flags, CLI_FLAG_AUTOCOMPLETE_ENABLED))
        return;

    AutocompletedCommand cmd = getAutocompletedCommand(cli, impl->cmdBuffer);

    if (cmd.candidateCount == 0) {
        cmd.autocompletedLen = impl->cmdSize;
    }

    // print live autocompletion (or nothing, if it doesn't exist)
    for (size_t i = impl->cmdSize; i < cmd.autocompletedLen; ++i) {
        cli->writeChar(cli, cmd.firstCandidate[i]);
    }
    // replace with spaces previous autocompletion
    for (size_t i = cmd.autocompletedLen; i < impl->inputLineLength; ++i) {
        cli->writeChar(cli, ' ');
    }
    impl->inputLineLength = cmd.autocompletedLen;
    cli->writeChar(cli, '\r');
    // print current command again so cursor is moved to initial place
    writeToOutput(cli, impl->invitation);
    writeToOutput(cli, impl->cmdBuffer);
}

static void onAutocompleteRequest(EmbeddedCli *cli) {
    PREPARE_IMPL(cli);

    AutocompletedCommand cmd = getAutocompletedCommand(cli, impl->cmdBuffer);

    if (cmd.candidateCount == 0)
        return;

    if (cmd.candidateCount == 1 || cmd.autocompletedLen > impl->cmdSize) {
        // can copy from index cmdSize, but prefix is the same, so copy everything
        memcpy(impl->cmdBuffer, cmd.firstCandidate, cmd.autocompletedLen);
        if (cmd.candidateCount == 1) {
            impl->cmdBuffer[cmd.autocompletedLen] = ' ';
            ++cmd.autocompletedLen;
        }
        impl->cmdBuffer[cmd.autocompletedLen] = '\0';

        writeToOutput(cli, &impl->cmdBuffer[impl->cmdSize]);
        impl->cmdSize = cmd.autocompletedLen;
        impl->inputLineLength = impl->cmdSize;
        return;
    }

    // with multiple candidates when we already completed to common prefix
    // we show all candidates and print input again
    // we need to completely clear current line since it begins with invitation
    clearCurrentLine(cli);

    for (int i = 0; i < impl->bindingsCount; ++i) {
        // autocomplete flag is set for all candidates by last call to
        // getAutocompletedCommand
        if (!(impl->bindingsFlags[i] & BINDING_FLAG_AUTOCOMPLETE))
            continue;

        const char *name = impl->bindings[i].name;

        writeToOutput(cli, name);
        writeToOutput(cli, lineBreak);
    }

    writeToOutput(cli, impl->invitation);
    writeToOutput(cli, impl->cmdBuffer);

    impl->inputLineLength = impl->cmdSize;
}

static void clearCurrentLine(EmbeddedCli *cli) {
    PREPARE_IMPL(cli);
    size_t len = impl->inputLineLength + strlen(impl->invitation);

    cli->writeChar(cli, '\r');
    for (size_t i = 0; i < len; ++i) {
        cli->writeChar(cli, ' ');
    }
    cli->writeChar(cli, '\r');
    impl->inputLineLength = 0;
}

static void writeToOutput(EmbeddedCli *cli, const char *str) {

    if (cli->writeString){
        cli->writeString(cli, str);
        return;
    }

    size_t len = strlen(str);

    for (size_t i = 0; i < len; ++i) {
        cli->writeChar(cli, str[i]);
    }
}

static bool isControlChar(char c) {
    return c == '\r' || c == '\n' || c == '\b' || c == '\t' || c == 0x7F;
}

static bool isDisplayableChar(char c) {
    return (c >= 32 && c <= 126);
}

static uint16_t fifoBufAvailable(FifoBuf *buffer) {
    if (buffer->back >= buffer->front)
        return (uint16_t) (buffer->back - buffer->front);
    else
        return (uint16_t) (buffer->size - buffer->front + buffer->back);
}

static char fifoBufPop(FifoBuf *buffer) {
    char a = '\0';
    if (buffer->front != buffer->back) {
        a = buffer->buf[buffer->front];
        buffer->front = (uint16_t) (buffer->front + 1) % buffer->size;
    }
    return a;
}

static bool fifoBufPush(FifoBuf *buffer, char a) {
    uint16_t newBack = (uint16_t) (buffer->back + 1) % buffer->size;
    if (newBack != buffer->front) {
        buffer->buf[buffer->back] = a;
        buffer->back = newBack;
        return true;
    }
    return false;
}

static bool historyPut(CliHistory *history, const char *str) {
    size_t len = strlen(str);
    // each item is ended with \0 so, need to have that much space at least
    if (history->bufferSize < len + 1)
        return false;

    // remove str from history (if it's present) so we don't get duplicates
    historyRemove(history, str);

    size_t usedSize;
    // remove old items if new one can't fit into buffer
    while (history->itemsCount > 0) {
        const char *item = historyGet(history, history->itemsCount);
        size_t itemLen = strlen(item);
        usedSize = ((size_t) (item - history->buf)) + itemLen + 1;

        size_t freeSpace = history->bufferSize - usedSize;

        if (freeSpace >= len + 1)
            break;

        // space not enough, remove last element
        --history->itemsCount;
    }
    if (history->itemsCount > 0) {
        // when history not empty, shift elements so new item is first
        memmove(&history->buf[len + 1], history->buf, usedSize);
    }
    memcpy(history->buf, str, len + 1);
    ++history->itemsCount;

    return true;
}

static const char *historyGet(CliHistory *history, uint16_t item) {
    if (item == 0 || item > history->itemsCount)
        return NULL;

    // items are stored in the same way (separated by \0 and counted from 1),
    // so can use this call
    return embeddedCliGetToken(history->buf, item);
}

static void historyRemove(CliHistory *history, const char *str) {
    if (str == NULL || history->itemsCount == 0)
        return;
    char *item = NULL;
    uint16_t itemPosition;
    for (itemPosition = 1; itemPosition <= history->itemsCount; ++itemPosition) {
        // items are stored in the same way (separated by \0 and counted from 1),
        // so can use this call
        item = embeddedCliGetTokenVariable(history->buf, itemPosition);
        if (strcmp(item, str) == 0) {
            break;
        }
        item = NULL;
    }
    if (item == NULL)
        return;

    --history->itemsCount;
    if (itemPosition == (history->itemsCount + 1)) {
        // if this is a last element, nothing is remaining to move
        return;
    }

    size_t len = strlen(item);
    size_t remaining = (size_t) (history->bufferSize - (item + len + 1 - history->buf));
    // move everything to the right of found item
    memmove(item, &item[len + 1], remaining);
}

static uint16_t getTokenPosition(const char *tokenizedStr, uint16_t pos) {
    if (tokenizedStr == NULL || pos == 0)
        return CLI_TOKEN_NPOS;
    uint16_t i = 0;
    uint16_t tokenCount = 1;
    while (true) {
        if (tokenCount == pos)
            break;

        if (tokenizedStr[i] == '\0') {
            ++tokenCount;
            if (tokenizedStr[i + 1] == '\0')
                break;
        }

        ++i;
    }

    if (tokenizedStr[i] != '\0')
        return i;
    else
        return CLI_TOKEN_NPOS;
}
