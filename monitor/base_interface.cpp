#include "base_interface.h"

#include <sys/wait.h>

#pragma GCC diagnostic ignored "-Wunused-result"

namespace monitor
{

namespace fs = std::filesystem;

static bool wdt_create_pipe([[maybe_unused]] int fd[2])
{
    // создадим pipe
    if (pipe(fd) < 0)
	{
		std::cerr << "pipe fail" << std::endl;
		return false;
	}
    return true;
}

static pid_t run_program([[maybe_unused]] fs::path path, [[maybe_unused]] const std::vector<std::string>& args)
{
    // подготавливаем аргументы для запуска
    std::vector<const char *> argv;
    argv.push_back(path.c_str());
    for (const std::string& arg : args)
    {
        argv.push_back(arg.c_str());
    }
    argv.push_back(NULL);

    posix_spawn_file_actions_t fileActions;

    int result = posix_spawn_file_actions_init(&fileActions);
    if (result != 0)
    {
        std::cerr << "posix_spawn_file_actions_init failed" << std::endl;
        return -1;
    }

    result = posix_spawn_file_actions_addclose(&fileActions, STDOUT_FILENO);
    if (result != 0)
    {
        std::cerr << "posix_spawn_file_actions_addclose failed" << std::endl;
        return -1;
    }

    posix_spawn_file_actions_t* pFileActions = &fileActions;

    posix_spawnattr_t attr;

    result = posix_spawnattr_init(&attr);
    if (result != 0)
    {
        std::cerr << "posix_spawnattr_init failed" << std::endl;
        return -1;
    }

    result = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    if (result != 0)
    {
        std::cerr << "posix_spawnattr_setflags failed" << std::endl;
        return -1;
    }

    sigset_t mask;
    sigfillset(&mask);
    result = posix_spawnattr_setsigmask(&attr, &mask);
    if (result != 0)
    {
        std::cerr << "posix_spawnattr_setsigmask failed" << std::endl;
        return -1;
    }
    posix_spawnattr_t* pAttr = &attr;

    pid_t childPid;
    result = posix_spawnp(&childPid, path.c_str(), pFileActions, pAttr, const_cast<char**>(&argv[0]),
        nullptr);
    if (result != 0)
    {
        std::cerr << "posix_spawn failed" << std::endl;
        return -1;
    }

    if (pAttr != NULL)
    {
        result = posix_spawnattr_destroy(pAttr);
        if (result != 0)
        {
            std::cerr << "posix_spawnattr_destroy failed" << std::endl;
            return -1;
        }
    }

    if (pFileActions != NULL) {
        result = posix_spawn_file_actions_destroy(pFileActions);
        if (result != 0)
        {
            std::cerr << "posix_spawn_file_actions_destroy failed" << std::endl;
            return -1;
        }
    }

    int status;
    result = waitpid(childPid, &status, WNOHANG);
    if (result == -1)
    {
        std::cerr << "waitpid failed" << std::endl;
        return -1;
    }
    return childPid;
}

static pid_t run_program(fs::path path)
{
    std::vector<std::string> empty;
    return run_program(path, empty);
}

//static
int IBaseInterface::m_wdtPipe[2] = { -1, -1 };

//static
void IBaseInterface::send_request(const pid_t pid)
{
    // запись pid процесса в пайп
    write(m_wdtPipe[1], &pid, sizeof(pid_t));
}

bool IBaseInterface::m_isTerminate = false;

/**
 * Конструктор
 */
IBaseInterface::IBaseInterface()
{
}

IBaseInterface::~IBaseInterface()
{
}

pid_t IBaseInterface::RunProgram(const t_path& path) const
{
    return run_program(path);
}

bool IBaseInterface::InitPipe()
{
    if (!wdt_create_pipe(m_wdtPipe))
    {
        return false;
    }
    OnCreateWdtPipe();
    return true;
}

pid_t IBaseInterface::RunProgram(const t_path& path, const t_args& args) const
{
    return run_program(path, args);
}

struct t_predefined_program
{
    pid_t pid;
    const fs::path path;
    const char *const args[10]; // аргументы командной строки
    bool watched;
};

/** Описание предопределенных программ для мониторинга */
static const t_predefined_program predefined_progs[] =
{
    {0, "some_app", {"-t25, -s1"}, true}, // пример
};

bool IBaseInterface::PreparePrograms()
{
    m_progs.clear();
    for ([[maybe_unused]] auto& it : predefined_progs)
    {
        // Создадим новую запись в списке наблюдаемых программ
        t_prog prog;
        prog.pid = it.pid;
        prog.path = it.path;
        prog.args = it.args;
        prog.watched = it.watched;

        // Добавим запись в список
        m_progs.push_back(prog);
    }
    return true;
}

bool IBaseInterface::TerminateProgram([[maybe_unused]] const pid_t pid) const
{
    // Проверим, что процесс существует
    if (kill(pid, 0) == -1) {
        return false;
    }

    // Отправим процессу сигнал SIGTERM
    kill(pid, SIGTERM);

    // Заждём завершения процесса
    int status = 0;
    waitpid(pid, &status, 0);

    // Если процесс завершился с ошибкой, вернём false
    if (WIFSIGNALED(status)) {
        return false;
    }
    return false;
}

pid_t IBaseInterface::FindTerminatedTask() const
{
    int pidstatus = 0;
    const pid_t pid = waitpid(-1, &pidstatus, WNOHANG);
    if (pid > 0)
    {
        if (0 == WIFSIGNALED(pidstatus))
        {
            pidstatus = WEXITSTATUS(pidstatus);
        }
        else
        {
            pidstatus = WTERMSIG(pidstatus);
        }
    }
    return pid;
}

bool IBaseInterface::GetRequestTask([[maybe_unused]] pid_t& pid) const
{
    // Считаем пид процесса, который пинговал из пайпа
    // Проверим, что канал связи существует
    if (m_wdtPipe[0] == -1) {
        return false;
    }

    // Прочитаем из канала связи PID процесса, который пинговал
    ssize_t n = read(m_wdtPipe[0], &pid, sizeof(pid_t));
    if (n != sizeof(pid_t)) {
        return false;
    }
    
    return true;
}

bool IBaseInterface::WaitExitAllPrograms() const
{
    pid_t pid = waitpid(-1, NULL, WNOHANG);
    while (pid > 0)
    {
        pid = waitpid(-1, NULL, WNOHANG);
    }

    return pid < 0;
}

bool IBaseInterface::ToDaemon() const
{
    // Демонизируем процесс мониторинга
    // Закроем стандартные потоки ввода/вывода
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Перейдем в корневой каталог
    chdir("/");

    // Запустим себя в фоновом режиме
    setsid();

    // Отключим сигнал SIGHUP
    signal(SIGHUP, SIG_IGN);

    return true;
}

void IBaseInterface::Destroy()
{
    // Закроем файловые дескрипторы пайпа
    if (m_wdtPipe[0] != -1) {
        close(m_wdtPipe[0]);
    }
    if (m_wdtPipe[1] != -1) {
        close(m_wdtPipe[1]);
    }
    m_isTerminate = false;
    
    m_init = false;
}

IBaseInterface::t_progs& IBaseInterface::Progs()
{
    return m_progs;
}

/**
 * Получить список наблюдаемых программ
 * @return список наблюдаемых программ
 */
const IBaseInterface::t_progs& IBaseInterface::Progs() const
{
    return m_progs;
}

} // namespace monitor
